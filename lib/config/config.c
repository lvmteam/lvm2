/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "lib.h"
#include "config.h"
#include "crc.h"
#include "pool.h"
#include "device.h"

#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <asm/page.h>

enum {
	TOK_INT,
	TOK_FLOAT,
	TOK_STRING,
	TOK_EQ,
	TOK_SECTION_B,
	TOK_SECTION_E,
	TOK_ARRAY_B,
	TOK_ARRAY_E,
	TOK_IDENTIFIER,
	TOK_COMMA,
	TOK_EOF
};

struct parser {
	char *fb, *fe;		/* file limits */

	int t;			/* token limits and type */
	char *tb, *te;

	int fd;			/* descriptor for file being parsed */
	int line;		/* line number we are on */

	struct pool *mem;
};

struct cs {
	struct config_tree cf;
	struct pool *mem;
	time_t timestamp;
	char *filename;
};

static void _get_token(struct parser *p);
static void _eat_space(struct parser *p);
static struct config_node *_file(struct parser *p);
static struct config_node *_section(struct parser *p);
static struct config_value *_value(struct parser *p);
static struct config_value *_type(struct parser *p);
static int _match_aux(struct parser *p, int t);
static struct config_value *_create_value(struct parser *p);
static struct config_node *_create_node(struct parser *p);
static char *_dup_tok(struct parser *p);

#define MAX_INDENT 32

#define match(t) do {\
   if (!_match_aux(p, (t))) {\
	log_error("Parse error at line %d: unexpected token", p->line); \
      return 0;\
   } \
} while(0);

static int _tok_match(const char *str, const char *b, const char *e)
{
	while (*str && (b != e)) {
		if (*str++ != *b++)
			return 0;
	}

	return !(*str || (b != e));
}

/*
 * public interface
 */
struct config_tree *create_config_tree(void)
{
	struct cs *c;
	struct pool *mem = pool_create(10 * 1024);

	if (!mem) {
		stack;
		return 0;
	}

	if (!(c = pool_alloc(mem, sizeof(*c)))) {
		stack;
		pool_destroy(mem);
		return 0;
	}

	c->mem = mem;
	c->cf.root = (struct config_node *) NULL;
	c->timestamp = 0;
	c->filename = NULL;
	return &c->cf;
}

void destroy_config_tree(struct config_tree *cf)
{
	pool_destroy(((struct cs *) cf)->mem);
}

int read_config_fd(struct config_tree *cf, int fd, const char *file,
		   off_t offset, size_t size, off_t offset2, size_t size2,
		   checksum_fn_t checksum_fn, uint32_t checksum)
{
	struct cs *c = (struct cs *) cf;
	struct parser *p;
	off_t mmap_offset = 0;
	int r = 0;

	if (!(p = pool_alloc(c->mem, sizeof(*p)))) {
		stack;
		return 0;
	}
	p->mem = c->mem;

	if (size2) {
		/* FIXME Attempt adjacent mmaps MAP_FIXED into malloced space 
		 * one PAGE_SIZE larger than required...
		 */
		if (!(p->fb = dbg_malloc(size + size2))) {
			stack;
			return 0;
		}
		if (lseek(fd, offset, SEEK_SET) < 0) {
			log_sys_error("lseek", file);
			goto out;
		}
		if (raw_read(fd, p->fb, size) != size) {
			log_error("Circular read from %s failed", file);
			goto out;
		}
		if (lseek(fd, offset2, SEEK_SET) < 0) {
			log_sys_error("lseek", file);
			goto out;
		}
		if (raw_read(fd, p->fb + size, size2) != size2) {
			log_error("Circular read from %s failed", file);
			goto out;
		}
	} else {
		mmap_offset = offset % PAGE_SIZE;
		/* memory map the file */
		p->fb = mmap((caddr_t) 0, size + mmap_offset, PROT_READ,
			     MAP_PRIVATE, fd, offset - mmap_offset);
		if (p->fb == (caddr_t) (-1)) {
			log_sys_error("mmap", file);
			goto out;
		}

		p->fb = p->fb + mmap_offset;
	}

	if (checksum_fn && checksum !=
	    (checksum_fn(checksum_fn(INITIAL_CRC, p->fb, size),
			 p->fb + size, size2))) {
		log_error("%s: Checksum error", file);
		goto out;
	}

	p->fe = p->fb + size + size2;

	/* parse */
	p->tb = p->te = p->fb;
	p->line = 1;
	_get_token(p);
	if (!(cf->root = _file(p))) {
		stack;
		goto out;
	}

	r = 1;

      out:
	if (size2)
		dbg_free(p->fb);
	else {
		/* unmap the file */
		if (munmap((char *) (p->fb - mmap_offset), size)) {
			log_sys_error("munmap", file);
			r = 0;
		}
	}

	return r;
}

int read_config_file(struct config_tree *cf, const char *file)
{
	struct cs *c = (struct cs *) cf;
	struct stat info;
	int r = 1, fd;

	if (stat(file, &info)) {
		log_sys_error("stat", file);
		return 0;
	}

	if (!S_ISREG(info.st_mode)) {
		log_error("%s is not a regular file", file);
		return 0;
	}

	if (info.st_size == 0) {
		log_verbose("%s is empty", file);
		return 1;
	}

	if ((fd = open(file, O_RDONLY)) < 0) {
		log_sys_error("open", file);
		return 0;
	}

	r = read_config_fd(cf, fd, file, 0, (size_t) info.st_size, 0, 0,
			   (checksum_fn_t) NULL, 0);

	close(fd);

	c->timestamp = info.st_mtime;
	c->filename = pool_strdup(c->mem, file);

	return r;
}

time_t config_file_timestamp(struct config_tree *cf)
{
	struct cs *c = (struct cs *) cf;

	return c->timestamp;
}

/*
 * Returns 1 if config file reloaded
 */
int reload_config_file(struct config_tree **cf)
{
	struct config_tree *new_cf;
	struct cs *c = (struct cs *) *cf;
	struct cs *new_cs;
	struct stat info;
	int r, fd;

	if (!c->filename)
		return 0;

	if (stat(c->filename, &info) == -1) {
		if (errno == ENOENT)
			return 1;
		log_sys_error("stat", c->filename);
		log_error("Failed to reload configuration file");
		return 0;
	}

	if (!S_ISREG(info.st_mode)) {
		log_error("Configuration file %s is not a regular file",
			  c->filename);
		return 0;
	}

	/* Unchanged? */
	if (c->timestamp == info.st_mtime)
		return 0;

	log_verbose("Detected config file change: Reloading %s", c->filename);

	if (info.st_size == 0) {
		log_verbose("Config file reload: %s is empty", c->filename);
		return 0;
	}

	if ((fd = open(c->filename, O_RDONLY)) < 0) {
		log_sys_error("open", c->filename);
		return 0;
	}

	if (!(new_cf = create_config_tree())) {
		log_error("Allocation of new config_tree failed");
		return 0;
	}

	r = read_config_fd(new_cf, fd, c->filename, 0, (size_t) info.st_size,
			   0, 0, (checksum_fn_t) NULL, 0);
	close(fd);

	if (r) {
		new_cs = (struct cs *) new_cf;
		new_cs->filename = pool_strdup(new_cs->mem, c->filename);
		new_cs->timestamp = info.st_mtime;
		destroy_config_tree(*cf);
		*cf = new_cf;
	}

	return r;
}

static void _write_value(FILE *fp, struct config_value *v)
{
	switch (v->type) {
	case CFG_STRING:
		fprintf(fp, "\"%s\"", v->v.str);
		break;

	case CFG_FLOAT:
		fprintf(fp, "%f", v->v.r);
		break;

	case CFG_INT:
		fprintf(fp, "%d", v->v.i);
		break;

	case CFG_EMPTY_ARRAY:
		fprintf(fp, "[]");
		break;

	default:
		log_error("_write_value: Unknown value type: %d", v->type);

	}
}

static int _write_config(struct config_node *n, FILE *fp, int level)
{
	char space[MAX_INDENT + 1];
	int l = (level < MAX_INDENT) ? level : MAX_INDENT;
	int i;

	if (!n)
		return 1;

	for (i = 0; i < l; i++)
		space[i] = '\t';
	space[i] = '\0';

	while (n) {
		fprintf(fp, "%s%s", space, n->key);
		if (!n->v) {
			/* it's a sub section */
			fprintf(fp, " {\n");
			_write_config(n->child, fp, level + 1);
			fprintf(fp, "%s}", space);
		} else {
			/* it's a value */
			struct config_value *v = n->v;
			fprintf(fp, "=");
			if (v->next) {
				fprintf(fp, "[");
				while (v) {
					_write_value(fp, v);
					v = v->next;
					if (v)
						fprintf(fp, ", ");
				}
				fprintf(fp, "]");
			} else
				_write_value(fp, v);
		}
		fprintf(fp, "\n");
		n = n->sib;
	}
	/* FIXME: add error checking */
	return 1;
}

int write_config_file(struct config_tree *cf, const char *file)
{
	int r = 1;
	FILE *fp = fopen(file, "w");
	if (!fp) {
		log_sys_error("open", file);
		return 0;
	}

	if (!_write_config(cf->root, fp, 0)) {
		stack;
		r = 0;
	}
	fclose(fp);
	return r;
}

/*
 * parser
 */
static struct config_node *_file(struct parser *p)
{
	struct config_node *root = NULL, *n, *l = NULL;
	while (p->t != TOK_EOF) {
		if (!(n = _section(p))) {
			stack;
			return 0;
		}

		if (!root)
			root = n;
		else
			l->sib = n;
		l = n;
	}
	return root;
}

static struct config_node *_section(struct parser *p)
{
	/* IDENTIFIER '{' VALUE* '}' */
	struct config_node *root, *n, *l = NULL;
	if (!(root = _create_node(p))) {
		stack;
		return 0;
	}

	if (!(root->key = _dup_tok(p))) {
		stack;
		return 0;
	}

	match(TOK_IDENTIFIER);

	if (p->t == TOK_SECTION_B) {
		match(TOK_SECTION_B);
		while (p->t != TOK_SECTION_E) {
			if (!(n = _section(p))) {
				stack;
				return 0;
			}

			if (!root->child)
				root->child = n;
			else
				l->sib = n;
			l = n;
		}
		match(TOK_SECTION_E);
	} else {
		match(TOK_EQ);
		if (!(root->v = _value(p))) {
			stack;
			return 0;
		}
	}

	return root;
}

static struct config_value *_value(struct parser *p)
{
	/* '[' TYPE* ']' | TYPE */
	struct config_value *h = NULL, *l, *ll = NULL;
	if (p->t == TOK_ARRAY_B) {
		match(TOK_ARRAY_B);
		while (p->t != TOK_ARRAY_E) {
			if (!(l = _type(p))) {
				stack;
				return 0;
			}

			if (!h)
				h = l;
			else
				ll->next = l;
			ll = l;

			if (p->t == TOK_COMMA)
				match(TOK_COMMA);
		}
		match(TOK_ARRAY_E);
		/*
		 * Special case for an empty array.
		 */
		if (!h) {
			if (!(h = _create_value(p)))
				return NULL;

			h->type = CFG_EMPTY_ARRAY;
		}

	} else
		h = _type(p);

	return h;
}

static struct config_value *_type(struct parser *p)
{
	/* [0-9]+ | [0-9]*\.[0-9]* | ".*" */
	struct config_value *v = _create_value(p);

	if (!v)
		return NULL;

	switch (p->t) {
	case TOK_INT:
		v->type = CFG_INT;
		v->v.i = strtol(p->tb, NULL, 0);	/* FIXME: check error */
		match(TOK_INT);
		break;

	case TOK_FLOAT:
		v->type = CFG_FLOAT;
		v->v.r = strtod(p->tb, NULL);	/* FIXME: check error */
		match(TOK_FLOAT);
		break;

	case TOK_STRING:
		v->type = CFG_STRING;

		p->tb++, p->te--;	/* strip "'s */
		if (!(v->v.str = _dup_tok(p))) {
			stack;
			return 0;
		}
		p->te++;
		match(TOK_STRING);
		break;

	default:
		log_error("Parse error at line %d: expected a value", p->line);
		return 0;
	}
	return v;
}

static int _match_aux(struct parser *p, int t)
{
	if (p->t != t)
		return 0;

	_get_token(p);
	return 1;
}

/*
 * tokeniser
 */
static void _get_token(struct parser *p)
{
	p->tb = p->te;
	_eat_space(p);
	if (p->tb == p->fe || !*p->tb) {
		p->t = TOK_EOF;
		return;
	}

	p->t = TOK_INT;		/* fudge so the fall through for
				   floats works */
	switch (*p->te) {
	case '{':
		p->t = TOK_SECTION_B;
		p->te++;
		break;

	case '}':
		p->t = TOK_SECTION_E;
		p->te++;
		break;

	case '[':
		p->t = TOK_ARRAY_B;
		p->te++;
		break;

	case ']':
		p->t = TOK_ARRAY_E;
		p->te++;
		break;

	case ',':
		p->t = TOK_COMMA;
		p->te++;
		break;

	case '=':
		p->t = TOK_EQ;
		p->te++;
		break;

	case '"':
		p->t = TOK_STRING;
		p->te++;
		while ((p->te != p->fe) && (*p->te) && (*p->te != '"')) {
			if ((*p->te == '\\') && (p->te + 1 != p->fe) &&
			    *(p->te + 1))
				p->te++;
			p->te++;
		}

		if ((p->te != p->fe) && (*p->te))
			p->te++;
		break;

	case '\'':
		p->t = TOK_STRING;
		p->te++;
		while ((p->te != p->fe) && (*p->te) && (*p->te != '\''))
			p->te++;

		if ((p->te != p->fe) && (*p->te))
			p->te++;
		break;

	case '.':
		p->t = TOK_FLOAT;
	case '0':
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	case '8':
	case '9':
		p->te++;
		while ((p->te != p->fe) && (*p->te)) {
			if (*p->te == '.') {
				if (p->t == TOK_FLOAT)
					break;
				p->t = TOK_FLOAT;
			} else if (!isdigit((int) *p->te))
				break;
			p->te++;
		}
		break;

	default:
		p->t = TOK_IDENTIFIER;
		while ((p->te != p->fe) && (*p->te) && !isspace(*p->te) &&
		       (*p->te != '#') && (*p->te != '=') && (*p->te != '{') &&
		       (*p->te != '}'))
			p->te++;
		break;
	}
}

static void _eat_space(struct parser *p)
{
	while ((p->tb != p->fe) && (*p->tb)) {
		if (*p->te == '#') {
			while ((p->te != p->fe) && (*p->te) && (*p->te != '\n'))
				p->te++;
			p->line++;
		}

		else if (isspace(*p->te)) {
			while ((p->te != p->fe) && (*p->te) && isspace(*p->te)) {
				if (*p->te == '\n')
					p->line++;
				p->te++;
			}
		}

		else
			return;

		p->tb = p->te;
	}
}

/*
 * memory management
 */
static struct config_value *_create_value(struct parser *p)
{
	struct config_value *v = pool_alloc(p->mem, sizeof(*v));
	memset(v, 0, sizeof(*v));
	return v;
}

static struct config_node *_create_node(struct parser *p)
{
	struct config_node *n = pool_alloc(p->mem, sizeof(*n));
	memset(n, 0, sizeof(*n));
	return n;
}

static char *_dup_tok(struct parser *p)
{
	size_t len = p->te - p->tb;
	char *str = pool_alloc(p->mem, len + 1);
	if (!str) {
		stack;
		return 0;
	}
	strncpy(str, p->tb, len);
	str[len] = '\0';
	return str;
}

/*
 * utility functions
 */
struct config_node *find_config_node(struct config_node *cn,
				     const char *path, const int sep)
{
	const char *e;

	while (cn) {
		/* trim any leading slashes */
		while (*path && (*path == sep))
			path++;

		/* find the end of this segment */
		for (e = path; *e && (*e != sep); e++) ;

		/* hunt for the node */
		while (cn) {
			if (_tok_match(cn->key, path, e))
				break;

			cn = cn->sib;
		}

		if (cn && *e)
			cn = cn->child;
		else
			break;	/* don't move into the last node */

		path = e;
	}

	return cn;
}

const char *find_config_str(struct config_node *cn,
			    const char *path, const int sep, const char *fail)
{
	struct config_node *n = find_config_node(cn, path, sep);

	if (n && n->v->type == CFG_STRING) {
		if (*n->v->v.str)
			log_very_verbose("Setting %s to %s", path, n->v->v.str);
		return n->v->v.str;
	}

	if (fail)
		log_very_verbose("%s not found in config: defaulting to %s",
				 path, fail);
	return fail;
}

int find_config_int(struct config_node *cn, const char *path,
		    const int sep, int fail)
{
	struct config_node *n = find_config_node(cn, path, sep);

	if (n && n->v->type == CFG_INT) {
		log_very_verbose("Setting %s to %d", path, n->v->v.i);
		return n->v->v.i;
	}

	log_very_verbose("%s not found in config: defaulting to %d",
			 path, fail);
	return fail;
}

float find_config_float(struct config_node *cn, const char *path,
			const int sep, float fail)
{
	struct config_node *n = find_config_node(cn, path, sep);

	if (n && n->v->type == CFG_FLOAT) {
		log_very_verbose("Setting %s to %f", path, n->v->v.r);
		return n->v->v.r;
	}

	log_very_verbose("%s not found in config: defaulting to %f",
			 path, fail);

	return fail;

}

static int _str_in_array(const char *str, const char *values[])
{
	int i;

	for (i = 0; values[i]; i++)
		if (!strcasecmp(str, values[i]))
			return 1;

	return 0;
}

static int _str_to_bool(const char *str, int fail)
{
	static const char *_true_values[] = { "y", "yes", "on", "true", NULL };
	static const char *_false_values[] =
	    { "n", "no", "off", "false", NULL };

	if (_str_in_array(str, _true_values))
		return 1;

	if (_str_in_array(str, _false_values))
		return 0;

	return fail;
}

int find_config_bool(struct config_node *cn, const char *path,
		     const int sep, int fail)
{
	struct config_node *n = find_config_node(cn, path, sep);
	struct config_value *v;

	if (!n)
		return fail;

	v = n->v;

	switch (v->type) {
	case CFG_INT:
		return v->v.i ? 1 : 0;

	case CFG_STRING:
		return _str_to_bool(v->v.str, fail);
	}

	return fail;
}

int get_config_uint32(struct config_node *cn, const char *path,
		      const int sep, uint32_t *result)
{
	struct config_node *n;

	n = find_config_node(cn, path, sep);

	if (!n || !n->v || n->v->type != CFG_INT)
		return 0;

	*result = n->v->v.i;
	return 1;
}

int get_config_uint64(struct config_node *cn, const char *path,
		      const int sep, uint64_t *result)
{
	struct config_node *n;

	n = find_config_node(cn, path, sep);

	if (!n || !n->v || n->v->type != CFG_INT)
		return 0;

	/* FIXME Support 64-bit value! */
	*result = (uint64_t) n->v->v.i;
	return 1;
}

int get_config_str(struct config_node *cn, const char *path,
		   const int sep, char **result)
{
	struct config_node *n;

	n = find_config_node(cn, path, sep);

	if (!n || !n->v || n->v->type != CFG_STRING)
		return 0;

	*result = n->v->v.str;
	return 1;
}
