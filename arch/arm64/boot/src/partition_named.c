/* Part of the loader (included by loader.c, not compiled alone): a partition of the phone's storage, by its name. */
static efi_block_io *
partition_named(const char *want)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	efi_handle *handles;
	uint64_t count;

	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) {
		return NULL;
	}
	for (uint64_t i = 0; i < count; i++) {
		efi_partition_info *info;
		efi_block_io *bio;
		char name[40];
		unsigned k = 0;

		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2 ||
		    EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio))) {
			continue;
		}
		ufs_name(info, name, sizeof(name));
		while (want[k] != 0 && name[k] == want[k]) {
			k++;
		}
		if (want[k] == 0 && name[k] == 0) {
			BS->free_pool(handles);
			return bio;
		}
	}
	BS->free_pool(handles);
	return NULL;
}
