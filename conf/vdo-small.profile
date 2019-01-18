# Demo configuration for 'VDO' using less memory.
# ~lvmconfig --type full | grep vdo

allocation {
	vdo_use_compression=1
	vdo_use_deduplication=1
	vdo_use_metadata_hints=1
	vdo_minimum_io_size=4096
	vdo_block_map_cache_size_mb=128
	vdo_block_map_period=16380
	vdo_check_point_frequency=0
	vdo_use_sparse_index=0
	vdo_index_memory_size_mb=256
	vdo_slab_size_mb=2048
	vdo_ack_threads=1
	vdo_bio_threads=1
	vdo_bio_rotation=64
	vdo_cpu_threads=2
	vdo_hash_zone_threads=1
	vdo_logical_threads=1
	vdo_physical_threads=1
	vdo_write_policy="auto"
	vdo_max_discard=1
}
