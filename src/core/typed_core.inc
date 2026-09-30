// Included by core.cpp after Core::Impl, sharing its transactional record store.
int Core::first_scan_v2(const CeScanRequestV2& r){return guarded([&]{return impl_->typed_scan(r,false,false);});}
int Core::next_scan_v2(const CeScanRequestV2& r){return guarded([&]{return impl_->typed_scan(r,true,false);});}
int Core::scan_info_v2(CeScanInfoV2& info){return guarded([&]{std::lock_guard lock(impl_->state_mutex);if(impl_->stopped.load())return impl_->stopped_error();CeScanInfoV2 result{sizeof(result),CE_V2_VERSION,0,0,0,0,0,0,impl_->generation,0};if(impl_->active)result=impl_->active->info;info=result;return int(CE_OK);});}
int Core::scan_status_v2(CeScanStatusV2& status) {
    return guarded([&] {
        std::unique_lock state(impl_->state_mutex, std::try_to_lock);
        if (!state.owns_lock()) return fail(CE_BUSY, L"Scan state is being committed or read; retry status later.");
        if (impl_->stopped.load()) return impl_->stopped_error();
        std::unique_lock telemetry(impl_->telemetry_mutex, std::try_to_lock);
        if (!telemetry.owns_lock()) return fail(CE_BUSY, L"Scan progress is being updated; retry status later.");
        auto snapshot = impl_->telemetry;
        snapshot.generation = impl_->generation;
        snapshot.has_scan = impl_->active ? 1u : 0u;
        snapshot.can_undo = impl_->undo ? 1u : 0u;
        if (snapshot.active) snapshot.cancel_requested = impl_->cancelled(impl_->telemetry_epoch) ? 1u : 0u;
        status = snapshot;
        return int(CE_OK);
    });
}
int Core::result_v2(uint64_t generation,uint64_t index,CeResultV2& info,std::vector<uint8_t>& bytes){return guarded([&]{std::lock_guard lock(impl_->state_mutex);if(impl_->stopped.load())return impl_->stopped_error();if(generation!=impl_->generation)return fail(CE_BUSY,L"The requested scan generation is stale.");if(!impl_->active)return fail(CE_INVALID_ARGUMENT,L"No active scan.");uint64_t address=0;std::vector<uint8_t> raw;int status=impl_->active->result(index,address,raw);if(status)return status;auto& m=impl_->active->info;CeResultV2 r{sizeof(r),CE_V2_VERSION,m.type,m.flags,m.byte_length,0,generation,address};info=r;bytes.swap(raw);return int(CE_OK);});}
int Core::new_scan_v2() {
    return guarded([&] {
        std::unique_lock scan(impl_->scan_mutex, std::try_to_lock);
        if (!scan.owns_lock()) return fail(CE_BUSY, L"Another scan is running.");
        std::lock_guard lock(impl_->state_mutex);
        if (impl_->stopped.load()) return impl_->stopped_error();
        if (impl_->generation == UINT64_MAX) return fail(CE_INTERNAL_ERROR, L"Scan generation exhausted.");
        impl_->active.reset();
        impl_->undo.reset();
        impl_->active_legacy = false;
        impl_->undo_legacy = false;
        ++impl_->generation;
        impl_->reset_scan_telemetry(CE_SCAN_IDLE);
        return int(CE_OK);
    });
}
int Core::undo_scan_v2() {
    return guarded([&] {
        std::unique_lock scan(impl_->scan_mutex, std::try_to_lock);
        if (!scan.owns_lock()) return fail(CE_BUSY, L"Another scan is running.");
        std::lock_guard lock(impl_->state_mutex);
        if (impl_->stopped.load()) return impl_->stopped_error();
        if (!impl_->undo) return fail(CE_INVALID_ARGUMENT, L"No previous scan generation to undo.");
        if (impl_->generation == UINT64_MAX) return fail(CE_INTERNAL_ERROR, L"Scan generation exhausted.");
        int status = impl_->undo->stamp(impl_->generation + 1);
        if (status != CE_OK) return status;
        impl_->active = std::move(impl_->undo);
        impl_->active_legacy = impl_->undo_legacy;
        impl_->undo_legacy = false;
        ++impl_->generation;
        impl_->reset_scan_telemetry(CE_SCAN_COMPLETED);
        return int(CE_OK);
    });
}
int Core::read_bytes_v2(uint64_t address,void* bytes,uint32_t length){return guarded([&]{std::lock_guard lock(impl_->state_mutex);if(impl_->stopped.load())return impl_->stopped_error();if(!bytes||!typed::valid_range(address,length))return fail(CE_INVALID_ARGUMENT,L"Invalid read address or byte length.");std::vector<uint8_t> data(length);if(!typed::read(address,data.data(),length))return fail(CE_ACCESS_ERROR,L"Cannot read host bytes.");std::memcpy(bytes,data.data(),length);return int(CE_OK);});}
int Core::write_bytes_v2(uint64_t address,const void* bytes,uint32_t length){return guarded([&]{std::lock_guard lock(impl_->state_mutex);if(impl_->stopped.load())return impl_->stopped_error();if(!bytes||!typed::valid_range(address,length))return fail(CE_INVALID_ARGUMENT,L"Invalid write address or byte length.");return typed::write(address,bytes,length)?int(CE_OK):fail(CE_ACCESS_ERROR,L"Raw write requires writable host memory.");});}
int Core::write_value_v2(uint64_t address,uint32_t type,uint32_t flags,uint32_t length,const wchar_t* value){return guarded([&]{std::lock_guard lock(impl_->state_mutex);if(impl_->stopped.load())return impl_->stopped_error();typed::Value parsed;int status=typed::parse(type,flags,length,value,parsed);if(status)return status;if(!typed::valid_range(address,parsed.width))return fail(CE_INVALID_ARGUMENT,L"Invalid typed write address.");return typed::write(address,parsed.bytes.data(),parsed.width)?int(CE_OK):fail(CE_ACCESS_ERROR,L"Typed write requires writable host memory.");});}
int Core::format_value_v2(uint32_t type,uint32_t flags,const void* bytes,uint32_t length,std::wstring& text){return guarded([&]{if(impl_->stopped.load())return impl_->stopped_error();return typed::format(type,flags,bytes,length,text);});}
int Core::resolve_address_v2(const wchar_t* expression,uint64_t& address){return guarded([&]{if(impl_->stopped.load())return impl_->stopped_error();return typed::expression(expression,address);});}
int Core::resolve_pointer_v2(const CeAddressV2& address,uint64_t& resolved){return guarded([&]{std::lock_guard lock(impl_->state_mutex);if(impl_->stopped.load())return impl_->stopped_error();return typed::resolve(address,resolved);});}
int Core::upsert_record_v2(const CeRecordRequestV2& r,uint64_t& id){return guarded([&]{std::lock_guard lock(impl_->state_mutex);if(impl_->stopped.load())return impl_->stopped_error();if(r.size!=sizeof(r)||r.version!=CE_V2_VERSION||r.reserved||r.address.reserved||r.address.offset_count>CE_V2_MAX_OFFSETS)return fail(CE_INVALID_ARGUMENT,L"Invalid typed record structure.");auto found=std::find_if(impl_->address_records.begin(),impl_->address_records.end(),[&](const auto& x){return x.stable_id==r.id;});if(r.id&&found==impl_->address_records.end())return fail(CE_INVALID_ARGUMENT,L"Record ID does not exist.");if(r.id&&found->ct_opaque)return fail(CE_UNSUPPORTED,L"Imported CT record type is not editable by this build.");StoredRecord record;if(found!=impl_->address_records.end())record=*found;record.type=r.type;record.flags=r.flags;record.target=r.address;record.ct_expression.clear();record.ct_signed_unspecified=false;record.description=r.description?r.description:L"";record.has_description=true;if(record.description.size()>max_description_chars)return fail(CE_INVALID_ARGUMENT,L"Description exceeds 4096 characters.");int status=typed::layout(r.type,r.flags,r.byte_length);if(status)return status;uint64_t resolved=0;int resolve_status=typed::resolve(r.address,resolved);if(resolve_status!=CE_OK)return resolve_status;
    if(r.value){typed::Value parsed;status=typed::parse(r.type,r.flags,r.byte_length,r.value,parsed);if(status)return status;record.width=parsed.width;record.raw=std::move(parsed.bytes);}else{record.width=r.byte_length?r.byte_length:typed::native_width(r.type);if(!record.width)return fail(CE_INVALID_ARGUMENT,L"Snapshot needs an explicit text/AOB width.");record.raw.resize(record.width);if(!typed::read(resolved,record.raw.data(),record.width))return fail(CE_ACCESS_ERROR,L"Cannot snapshot record value.");}
    if(!typed::valid_range(resolved,record.width))return fail(CE_INVALID_ARGUMENT,L"Record range overflows the host address space.");record.record.address=resolved;record.record.value=0;if(record.width==4)std::memcpy(&record.record.value,record.raw.data(),4);record.last_status=CE_OK;record.raw_valid=true;
    if(record.record.frozen&&(!typed::accessible(resolved,record.width,true)||impl_->conflict(resolved,record.width,r.id)))return fail(CE_INVALID_ARGUMENT,L"Edited enabled freeze has an inaccessible or overlapping byte range.");
    if(r.id){*found=std::move(record);id=r.id;}else{if(impl_->address_records.size()>=max_table_records)return fail(CE_UNSUPPORTED,L"Record limit is 10000.");record.stable_id=impl_->allocate_id();id=record.stable_id;impl_->address_records.push_back(std::move(record));}++impl_->revision;return int(CE_OK);});}
int Core::record_count_v2(uint64_t& count){return guarded([&]{std::lock_guard lock(impl_->state_mutex);if(impl_->stopped.load())return impl_->stopped_error();count=impl_->address_records.size();return int(CE_OK);});}
int Core::record_v2(uint64_t index,CeRecordInfoV2& info,std::wstring& description){return guarded([&]{std::lock_guard lock(impl_->state_mutex);if(impl_->stopped.load())return impl_->stopped_error();if(index>=impl_->address_records.size())return fail(CE_INVALID_ARGUMENT,L"Record index is out of range.");auto& r=impl_->address_records[size_t(index)];// Opaque records must not reach resolve: a dangling chain would overwrite the
    // unsupported marker with CE_ACCESS_ERROR and hide why the row is read-only.
    if(r.ct_opaque){CeRecordInfoV2 opaque{sizeof(opaque),CE_V2_VERSION,r.type,0,0,0,CE_UNSUPPORTED,0,r.stable_id,0,r.location()};std::wstring text=r.description;info=opaque;description.swap(text);return int(CE_OK);}uint64_t address=0;int status=typed::resolve(r.location(),address);CeRecordInfoV2 output{sizeof(output),CE_V2_VERSION,r.type,r.flags,r.width,r.record.frozen?1u:0u,status==CE_OK?r.last_status:uint32_t(status),0,r.stable_id,address,r.location()};std::wstring text=r.description;info=output;description.swap(text);return int(CE_OK);});}
int Core::remove_record_v2(uint64_t id){return guarded([&]{std::lock_guard lock(impl_->state_mutex);if(impl_->stopped.load())return impl_->stopped_error();auto found=std::find_if(impl_->address_records.begin(),impl_->address_records.end(),[&](const auto& x){return x.stable_id==id;});if(found==impl_->address_records.end())return fail(CE_INVALID_ARGUMENT,L"Record ID does not exist.");impl_->address_records.erase(found);++impl_->revision;return int(CE_OK);});}
int Core::write_record_v2(uint64_t id, const wchar_t* value) {
    return guarded([&] {
        std::lock_guard lock(impl_->state_mutex);
        if (impl_->stopped.load()) return impl_->stopped_error();
        auto found = std::find_if(impl_->address_records.begin(), impl_->address_records.end(),
            [&](const auto& item) { return item.stable_id == id; });
        if (found == impl_->address_records.end()) return fail(CE_INVALID_ARGUMENT, L"Record ID does not exist.");
        if (found->ct_opaque) return fail(CE_UNSUPPORTED, L"Imported CT record type is not editable by this build.");
        typed::Value parsed;
        int status = typed::parse(found->type, found->flags, found->width, value, parsed);
        if (status != CE_OK) return status;
        uint64_t address = 0;
        status = typed::resolve(found->location(), address);
        // A pointer may have moved since the last tick. Check before the write,
        // preserving both host memory and the saved freeze bytes on collision.
        if (status == CE_OK && found->record.frozen && impl_->conflict(address, parsed.width, id))
            status = CE_INVALID_ARGUMENT;
        if (status == CE_OK && !typed::write(address, parsed.bytes.data(), parsed.width)) status = CE_ACCESS_ERROR;
        if (status != CE_OK) {
            found->last_status = status;
            found->record.frozen = false;
            ++impl_->revision;
            return fail(status, L"Cannot write resolved record; freeze disabled.");
        }
        found->raw = std::move(parsed.bytes);
        found->raw_valid = true;
        if (found->width == 4) std::memcpy(&found->record.value, found->raw.data(), 4);
        found->record.address = address;
        found->last_status = CE_OK;
        ++impl_->revision;
        return int(CE_OK);
    });
}
int Core::freeze_record_v2(uint64_t id,bool enabled){return guarded([&]{std::lock_guard lock(impl_->state_mutex);if(impl_->stopped.load())return impl_->stopped_error();auto found=std::find_if(impl_->address_records.begin(),impl_->address_records.end(),[&](const auto& x){return x.stable_id==id;});if(found==impl_->address_records.end())return fail(CE_INVALID_ARGUMENT,L"Record ID does not exist.");if(found->ct_opaque)return fail(CE_UNSUPPORTED,L"Imported CT record type is not editable by this build.");if(enabled){uint64_t address=0;int status=typed::resolve(found->location(),address);if(status)return status;if(!typed::accessible(address,found->width,true))return fail(CE_ACCESS_ERROR,L"Freeze needs a currently writable byte range.");if(impl_->conflict(address,found->width,id))return fail(CE_INVALID_ARGUMENT,L"Enabled freeze byte ranges cannot overlap.");if(!found->raw_valid){std::vector<uint8_t> snapshot(found->width);if(!typed::read(address,snapshot.data(),found->width))return fail(CE_ACCESS_ERROR,L"Imported record has no valid value; cannot snapshot current bytes for freezing.");found->raw.swap(snapshot);found->raw_valid=true;if(found->width==4)std::memcpy(&found->record.value,found->raw.data(),4);}found->record.address=address;found->last_status=CE_OK;}found->record.frozen=enabled;++impl_->revision;return int(CE_OK);});}
