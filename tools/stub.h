/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#define unimplemented \
	{ log_error("Command not implemented yet."); return ECMD_FAILED;}

/*int e2fsadm(struct cmd_context *cmd, int argc, char **argv) unimplemented*/
int lvmsadc(struct cmd_context *cmd, int argc, char **argv) unimplemented
int lvmsar(struct cmd_context *cmd, int argc, char **argv) unimplemented
int pvresize(struct cmd_context *cmd, int argc, char **argv) unimplemented

int pvdata(struct cmd_context *cmd, int argc, char **argv) {
	log_error("There's no 'pvdata' command in LVM2.");
	log_error("Use lvs, pvs, vgs instead; or use vgcfgbackup and read the text file backup.");
	log_error("Metadata in LVM1 format can still be displayed using LVM1's pvdata command.");
	return ECMD_FAILED;
}

