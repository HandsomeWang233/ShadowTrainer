// The pointer scan reuses the value scan's single-job admission and cancellation epoch,
// so only one of the two can run at a time and one Cancel reaches whichever is running.
// Everything else about it is separate: its own telemetry slot, its own generation, its
// own result set. Touching the value scan's `active`/`undo`/`generation` here would break
// CE_GetResultV2's staleness contract and make the UI repaint the value results.
int Core::pointer_scan_v2(const CePointerScanRequestV2& request) {
    return guarded([&] {
        const auto epoch = impl_->cancellation.load();
        std::unique_lock scan_lock(impl_->scan_mutex, std::try_to_lock);
        if (!scan_lock.owns_lock()) return fail(CE_BUSY, L"Another scan is already running.");
        if (impl_->stopped.load()) return impl_->stopped_error();
        const uint64_t operation = ++impl_->pointer_operation;
        {
            std::lock_guard lock(impl_->telemetry_mutex);
            // Keep the published generation: a failed scan must not hide the previous
            // result set from readers.
            const uint64_t published = impl_->pointer_telemetry.generation;
            impl_->pointer_telemetry = {sizeof(CePointerScanStatusV2), CE_V2_VERSION};
            impl_->pointer_telemetry.operation_id = operation;
            impl_->pointer_telemetry.generation = published;
            impl_->pointer_telemetry.phase = CE_SCAN_ENUMERATING;
            impl_->pointer_telemetry.active = 1;
        }
        const auto finish = [&](int status) {
            std::lock_guard lock(impl_->telemetry_mutex);
            impl_->pointer_telemetry.active = 0;
            impl_->pointer_telemetry.cancel_requested = impl_->cancelled(epoch) ? 1u : 0u;
            impl_->pointer_telemetry.phase = status == CE_OK ? CE_SCAN_COMPLETED :
                status == CE_CANCELLED ? CE_SCAN_CANCELLED : CE_SCAN_FAILED;
            return status;
        };

        std::vector<CeRegionInfoV2> regions;
        if (const int status = walk_regions(regions)) return finish(status);
        std::vector<pointerscan::Range> ranges;
        for (const auto& region : regions) {
            if (region.state != MEM_COMMIT) continue;
            if (region.protect & (PAGE_GUARD | PAGE_NOACCESS)) continue;
            switch (region.protect & 255) {
            case PAGE_READONLY: case PAGE_READWRITE: case PAGE_WRITECOPY:
            case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY:
                ranges.push_back({region.base, region.base + region.region_size});
                break;
            default: break;
            }
        }
        std::vector<CeModuleInfoV2> loaded;
        if (const int status = walk_modules(loaded)) return finish(status);
        std::vector<pointerscan::Range> modules;
        for (const auto& module : loaded) modules.push_back({module.base, module.base + module.module_size});

        pointerscan::Outcome outcome;
        const int status = pointerscan::scan(request, ranges, modules,
            [&] { return impl_->cancelled(epoch); },
            [&](const pointerscan::Progress& progress) {
                std::lock_guard lock(impl_->telemetry_mutex);
                impl_->pointer_telemetry.phase = progress.phase;
                impl_->pointer_telemetry.processed = progress.processed;
                impl_->pointer_telemetry.total = progress.total;
            },
            [&](const CePointerScanResultV2& result) {
                CeAddressV2 chain{};
                chain.base = result.base;
                chain.offset_count = result.level_count;
                for (uint32_t i = 0; i < result.level_count; ++i) chain.offsets[i] = result.offsets[i];
                uint64_t resolved = 0;
                return typed::resolve(chain, resolved) == CE_OK && resolved == request.target;
            }, outcome);
        {
            std::lock_guard lock(impl_->telemetry_mutex);
            impl_->pointer_telemetry.truncated = outcome.truncated ? 1u : 0u;
            // processed counts harvested pointer slots, total the resulting paths.
            impl_->pointer_telemetry.processed = outcome.slots;
            impl_->pointer_telemetry.total = outcome.results.size();
        }
        if (status != CE_OK) return finish(status);
        {
            std::lock_guard lock(impl_->state_mutex);
            impl_->pointer_results = std::move(outcome.results);
            ++impl_->pointer_generation;
            std::lock_guard telemetry(impl_->telemetry_mutex);
            impl_->pointer_telemetry.generation = impl_->pointer_generation;
        }
        return finish(CE_OK);
    });
}
int Core::pointer_scan_result_v2(uint64_t generation, uint64_t index, CePointerScanResultV2& result) {
    return guarded([&] {
        std::lock_guard lock(impl_->state_mutex);
        if (impl_->stopped.load()) return impl_->stopped_error();
        if (generation != impl_->pointer_generation) return fail(CE_BUSY, L"Pointer scan results changed; retry.");
        if (index >= impl_->pointer_results.size()) return fail(CE_INVALID_ARGUMENT, L"Pointer scan result index is out of range.");
        result = impl_->pointer_results[size_t(index)];
        return int(CE_OK);
    });
}
int Core::pointer_scan_status_v2(CePointerScanStatusV2& status) {
    if (impl_->stopped.load()) return impl_->stopped_error();
    std::unique_lock state(impl_->state_mutex, std::try_to_lock);
    if (!state.owns_lock()) return CE_BUSY;
    std::unique_lock telemetry(impl_->telemetry_mutex, std::try_to_lock);
    if (!telemetry.owns_lock()) return CE_BUSY;
    status = impl_->pointer_telemetry;
    return CE_OK;
}
