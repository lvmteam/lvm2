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
int pvdata(struct cmd_context *cmd, int argc, char **argv) unimplemented
int pvresize(struct cmd_context *cmd, int argc, char **argv) unimplemented
int vgmknodes(struct cmd_context *cmd, int argc, char **argv) unimplemented
