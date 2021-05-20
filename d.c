#include <stdio.h>
#include <stdarg.h>
#include <regex.h>
#include <ctype.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <setjmp.h>


/* COMMANDS:
 * a append at a range
 * c change a range
 * d delete a range
 * e open a file
 * E edit unconditionally
 * g global /RE/command-list
 * i append before
 * j join lines
 * kx mark at x
 * q quit
 * Q unconditional q
 * r read
 * ! shell
 * t transfer/yank/copy
 * u undo
 * w [!|q]
 * W noclobber w
 * # comment/set address
 */

#define EDPROMPT ":"

/* Max replacements in a string */
#define REPLIM 200

/* 
 * Maximum marks 
 * From '!' (dec 33) to '~' (dec 126)
 * 126 - 33 = 93
 */
#define MARKLIM 93 

jmp_buf torepl;

typedef struct node {
	struct node *prev;
	char *s;
	struct node *next;
}node_t;

typedef struct regbuf {
	node_t **buf;
	int size;
}regbuf_t;


typedef struct {
	char cmd;
	node_t *from;
	int to;
	char *rest;
	char mark;
	char *regex;	
}eval_t;

/* length of the global list */
long gbl_len;

node_t *gbl_head_node;
node_t *gbl_tail_node;
node_t *gbl_current_node;
long gbl_current_ptr;


typedef struct {
	char *filename;
	bool saved;
	char *cmd;
	bool fromfile;
}state_t;
state_t state;

/* Mark array */
node_t *gbl_marks[MARKLIM];

/** List **/
node_t *ll_add(node_t *head, char *s, int at);
node_t *ll_add_begin(node_t *head, char *s);
node_t *ll_add_end(node_t *head, char *s);
node_t *ll_remove(node_t *head, int at);
node_t *ll_new_node(char *s);
node_t * ll_at(int at);
void ll_free_node(node_t **node);
node_t * ll_prev_node(node_t *node, int n);
node_t * ll_next_node(node_t *node, int n);
/* For debugging */
void ll_print(node_t *head); 
node_t *ll_add_node(node_t *node, char *s);

/* parse routines & eval routines */
eval_t * parse(char *exp, eval_t *ev);
char *skipspaces(char *s);
eval_t *parse_address(eval_t *ev, char *addr);

void ed_save(char *filename, char *cmd, bool quit, bool append);
void ed_quit(bool force);
void ed_subs(node_t *from, int to, char *regex, char *rest);
void ed_print(node_t *from, int to);
void ed_read(char *filename, char *cmd, node_t *from);
void ed_join(node_t *from, int to);

void die(char *file, int line, char *fn, char *cause);
void io_err(const char *fmt, ...);
node_t * io_load_file(FILE *fp);
int io_write_file(node_t *head, char *filename, char *mode);
void io_reg_err(regex_t *regcmp, int errcode);
FILE *fileopen(char *filename, char *mode);

int markset(node_t *node, int at);
node_t *markget(int at);
void markclear(int at);
void ed_mark(node_t *node, int rest);

/* Mark functions */
int markset(node_t *node, int at) {
	int i = at - '!';
	gbl_marks[i] = node;
	return i;
}

node_t *markget(int at) {
	node_t *r = gbl_marks[at - '!'];
	return r;
}

void markclear(int at) {
	gbl_marks[at - '!'] = NULL;
}

void ll_print(node_t *head) {
	node_t *tptr = head;
	while (tptr != NULL) {
		printf("%s", tptr->s);
		tptr = tptr->next;
	}
}

node_t *ll_add(node_t *head, char *s, int at) {
	state.saved = false;
	if (head == NULL || at == 0) {
		return ll_add_begin(gbl_head_node, s);
	}

	node_t *newnode = ll_new_node(s);

	if (at == gbl_len) {
		return ll_add_end(gbl_tail_node, s);
	}
	node_t *current = head;
	for (int i = 0; current->next != NULL; ++i, current = current->next) {
		if (i == at) {
			newnode->prev = current->prev;
			newnode->next = current;
			current->prev->next = newnode;
			current->prev = newnode;
			gbl_current_ptr = i;
			break;
		}
	}
	gbl_len++;
	return newnode;
}

node_t *ll_new_node(char *s) {
	node_t *node = (node_t *) calloc(1, sizeof(node_t));

	node->s = (char *) calloc(strlen(s), sizeof(char));
	strcpy(node->s, s);

	node->next = NULL;
	node->prev = NULL;
	return node;
}

void ll_free_node(node_t **node) {
	free((node_t *) *node);
	(*node) = NULL;
}

node_t *ll_add_begin(node_t *head, char *s) {
	state.saved = false;
	if (s == NULL)
		die(__FILE__, __LINE__, "ll_add_begin", "Empty data string");

	node_t *newnode = ll_new_node(s);

	if (head == NULL) {
		gbl_head_node = gbl_current_node = gbl_tail_node = newnode;
	}
	else if (head != NULL) {
		newnode->next = head;
		head->prev = newnode;
		gbl_head_node = newnode;
		gbl_current_node = newnode;
	}
	gbl_len++;
	gbl_current_ptr = 0;
	return newnode;
}

node_t *ll_add_end(node_t *tail, char *s) {
	state.saved = false;
	if (s == NULL)
		die(__FILE__, __LINE__, "ll_add_end", "Empty data string");

	if (tail == NULL) {
		return ll_add_begin(tail, s);
	}
	else if (tail != NULL) {
		node_t *newnode = ll_new_node(s);
		tail->next = newnode;
		newnode->prev = tail;
		gbl_tail_node = newnode;
		gbl_len++;
		gbl_current_ptr = gbl_len;
		return newnode;
	}
	die(__FILE__, __LINE__, "ll_add_end", "No viable selection");
	return NULL;
}

node_t *ll_remove(node_t *head, int at) {
	state.saved = false;
	node_t *rmnode;
	if (at == 0) {
		rmnode = gbl_head_node;
		gbl_head_node = gbl_head_node->next;
		gbl_head_node->prev = NULL;
		gbl_current_node = gbl_head_node;
		gbl_current_ptr = 0;
		ll_free_node(&rmnode);
		gbl_len--;
		return gbl_head_node;
	}
	else if (at == gbl_len) {
		rmnode = gbl_tail_node;
		gbl_tail_node = gbl_tail_node->prev;
		gbl_tail_node->next = NULL;
		gbl_current_node = gbl_tail_node;
		ll_free_node(&rmnode);
		gbl_len--;
		gbl_current_ptr = gbl_len;
		return gbl_tail_node;
	}
	else if (at > 0 && at < gbl_len) {
		node_t * current = head;
		for (int i = 0; current->next != NULL; ++i, current = current->next) {
			if (i == at) {
				node_t *back = current->prev;
				node_t *front = current->next;
				back->next = current->next;
				front->prev = back;
				ll_free_node(&current);
				gbl_len--;
				gbl_current_ptr = i;
				return front;
			}
		}
	}
	die(__FILE__, __LINE__, "ll_remove", "Index Out of Range");
	return NULL;
}

node_t * ll_at(int at) {
	node_t *current = gbl_head_node;
	for (int i = 0; current != NULL; ++i, current = current->next) {
		if (i == at) {
			return current;
		}
	}
	return NULL;
}

int ll_at_i(node_t *node) {
	node_t *current = gbl_head_node;
	int i = 0;
	while (current != NULL) {
		if (current == node) {
			return i;
		}
		++i;
		current = current->next;
	}
	return -1;
}

node_t * ll_remove_node(node_t * node) {
	state.saved = false;
	if (node == gbl_head_node) {
		gbl_head_node = node->next;
		gbl_head_node->prev = NULL;
		gbl_len++;
		gbl_current_node = 0;
		ll_free_node(&node);
		return gbl_head_node;
	}
	else if (node == gbl_tail_node) {
		gbl_tail_node = node->prev;
		gbl_tail_node->next = NULL;
		gbl_len++;
		gbl_current_ptr = gbl_len;
		ll_free_node(&node);
		return gbl_tail_node;
	}
	else {
		node_t *back = node->prev;
		node_t *front = node->next;
		gbl_current_node = front;
		front->prev = back;
		back->next = front;
		gbl_current_ptr = ll_at_i(front);
		gbl_len++;
		ll_free_node(&node);
		return front;
	}
	return NULL;
}


void ll_free(node_t *head) {
	node_t *current = head;
	while (current != NULL) {
		node_t *rmnode = current;
		current = current->next;
		ll_free_node(&rmnode);
	}
	gbl_head_node = NULL;
	gbl_tail_node = NULL;
	gbl_len = 0;
	gbl_current_node = NULL;
}

regbuf_t *
ll_reg_search(node_t *node, int offset, const char *regpattern) {
	node_t *current = node;
	regex_t reg;
	int ret;

	regbuf_t *rbuf = (regbuf_t *) calloc(1, sizeof(regbuf_t));
	rbuf->buf = (node_t **) calloc(offset, sizeof(node_t *));
	rbuf->size = 0;

	if ((ret = regcomp(&reg, regpattern, REG_NOSUB)) != 0) {
		io_reg_err(&reg, ret);
		return NULL;
	}

	for (int i = 0; i < offset || current != NULL; ++i, current = current->next) {
		if ((ret = regexec(&reg, current->s, 0, NULL, 0)) == 0) {
			rbuf->buf[rbuf->size] = current;
			rbuf->size++;
		}
	}
	regfree(&reg);
	return rbuf;
}

node_t * ll_prev_node(node_t *node, int n) {
	for (; n != 1 || node != NULL; --n, node = node->prev);
	return node;
}

node_t * ll_next_node(node_t *node, int n) {
	for (int i = 0; i < n && node != NULL; ++i, node = node->next);
	return node;
}

void io_reg_err(regex_t *regcmp, int errcode) {
	char buf[200];
	regerror(errcode, regcmp, buf, 200);
	fprintf(stderr, "%s", buf);
	longjmp(torepl, 1);
}

FILE *fileopen(char *filename, char *mode) {
	FILE *fp;
	if ((fp = fopen(filename, mode)) == NULL) {
		perror("fopen");
		return NULL;
	}
	state.fromfile = true;
	state.filename = filename;
	return fp;
}

node_t * io_load_file(FILE *fp) {
	state.saved = true;
	char *line = NULL;
	size_t linecap;
	ssize_t total_lines_read = 0;
	node_t *head = NULL;

	while ((getline(&line, &linecap, fp)) > 0) {
		head = ll_add_end(head, line);
		total_lines_read++;
	}

	printf("%ld line%s read from \"%s\"\n", total_lines_read,
			(total_lines_read==1)?"":"s", 
			(state.fromfile) ? state.filename : state.cmd);

	free(line);
	fclose(fp);
	return head;
}

int io_write_file(node_t *head, char *filename, char *mode) {
	FILE *fp = fileopen(filename, mode);

	size_t lines = 0;

	node_t *current = head;
	while(current != NULL) {
		fprintf(fp, "%s", current->s);
		current = current->next;
		lines++;
	}
	printf("%ld line%s written to \"%s\"\n", lines,
		   (lines==1)?"":"s", filename);
	fclose(fp);
	return 0;
}

char *io_read_line(char *prompt) {
	char *line = NULL;
	size_t linecap = 0;

	if (prompt != NULL) {
		printf("%s", prompt);
	}
	if (getdelim(&line, &linecap, '\n', stdin) != -1)
		return line;
	return NULL;
}

void die(char *file, int line, char *fn, char *cause) {
	bool nocause = false;
	if (cause == NULL) { nocause = true; }

	fprintf(stderr, "%s:%d - %s: %s\n",
			file, line, fn, (nocause ? strerror(errno): cause));
	exit(EXIT_FAILURE);
}

const char * commandchars = "acdeEgijklmnpqQrsw!";
const char * addressbasedcommands = "acdgijklmnpqQrs";
const char * filebasedcommands = "eEw!";

char *skipspaces(char *s) {
	if (! isspace(*s))
		return s;
	while (isspace(*s))
		s++;
	return s;
}

char *nextword(char *s) {
	while (! isspace(*s) && *s != '\0') 
		++s;
	while(isspace(*s) && *s != '\0')
		++s;
	return s;
}

int iscommand(char * cmd) {
	char *start = cmd;
	//while (! isspace(*cmd)) {
	//	cmd++;
	//}
	if ( /*((cmd - start) == 1) &&*/ strchr(commandchars, *start)) {
		return 1;
	}
	return 0;
}

char *rmnewline(char *s) {
	char *start = s;
	while (*s != '\n')
		s++;
	*s = '\0';
	return start;
}

eval_t *parse_address(eval_t *ev, char *addr) {
	char *naddr = addr;
	if (*naddr == '"') {
		naddr++;
		while (*naddr != '"') { naddr++; }
	}
	else {
		while (! isspace(*naddr) && *naddr != '\0') { naddr++; }
	}
	
	int size = naddr - addr;

	bool commapassed = false;
	while (addr < naddr) {
		if (*addr == '.') {
			if (commapassed) { ev->to = gbl_current_ptr; }
			else { ev->from = gbl_current_node; }
		}
		else if (*addr == '$') {
			if (commapassed) { ev->to = gbl_len; }
			else { ev->from = gbl_tail_node; }
		}
		else if (*addr == ',') {
			if (size == 1) {
				ev->from = gbl_head_node;
				ev->to = gbl_len;
			}
			else {
				commapassed = true;
			}
		}
		else if (*addr == '-') {
			long num = 1;
			if (isdigit(*(addr+1))) {
				num = strtol(addr + 1, &addr, 10);
			}

			if (commapassed) {
				ev->to = gbl_current_ptr - num;
		   	}
			else {
				ev->from = ll_prev_node(gbl_current_node, num);
			}
			addr--;
		}
		else if (*addr == '+') {
			long num = 1;
			if (isdigit(*(addr+1))) {
				num = strtol(addr + 1, &addr, 10);
			}

			if (commapassed) {
				ev->to = gbl_current_ptr + num;
			}
			else {
				ev->from = ll_next_node(gbl_current_node, num);
			}
			addr--;
		}
		else if (isdigit(*addr)) {
			long num = strtol(addr, &addr, 10);
			if (commapassed) {
				ev->to = num;
			}
			else {
				ev->from = ll_at(num);
			}
			addr--;
		}
		else if (*addr == ';') {
			ev->from = gbl_current_node;
			ev->to = gbl_len;
		}
		else if (*addr == '/') {
			char *start = addr+1;
			addr++;
			while (*addr != '/' && *(addr-1) != '\\') {
				addr++;
			}
			*addr = '\0';
			ev->regex = start;
			break;
		}
		else if (*addr == '\'') {
			if ((ev->from = markget(*(addr+1))) == NULL) 
				io_err("Mark not set %c", *(addr+1));

		}
		addr++;
	}
	return ev;
}

void eval_defaults(eval_t *ev) {
	ev->from = gbl_current_node;
	ev->to = gbl_len;
}
	

eval_t * parse(char *exp, eval_t *ev) {
	eval_defaults(ev);
	exp = rmnewline(exp);
	exp = skipspaces(exp);
	char *address = nextword(exp);
	char *rest = nextword(address);

	if (iscommand(exp)) {
		ev->cmd = exp[0];
		if (strchr(addressbasedcommands, exp[0])) {
			parse_address(ev, address);
			ev->rest = rest;
		}
		else if (strchr(filebasedcommands, exp[0])) {
			ev->rest = address;
		}
		return ev;
	}
	else {
		io_err("Unknown command: %s", exp);
		return NULL; // Never reached
	}
}



node_t * ed_append(node_t * node) {
	char *line = NULL;
	size_t bytes = 0;
	size_t lines = 0;
	size_t linecap;
	while ((bytes = getline(&line, &linecap, stdin)) > 0) {
		if (line[0] == '.')
			break;
		node = ll_add_node(node, line);
		lines++;
		bytes += bytes;
	}
	printf("%ld line%s appended\n", lines, (lines==1)?"":"s");
	return gbl_current_node;
}

node_t * ed_delete(node_t *from, int to) {
	for (int i = 0; i < to; ++i) {
		from = ll_remove_node(from);
	}
	return from;
}

node_t * ed_change(node_t *from, int to) {
	node_t *start = from->prev;
	ed_delete(from, to);
	return ed_append(start);
}

void io_print_file(FILE *fp) {
	char *line = NULL;
	size_t linecap;
	while (getline(&line, &linecap, fp) > 0) {
		printf("%s", line);
	}
	free(line);
}


FILE * ed_shell(char *cmd, bool out) {
	FILE *fp;
	if ((fp = popen(cmd, "r")) == NULL) {
		perror("popen");
	}

	state.fromfile = false;
	state.cmd = cmd;
	if (out) {
		io_print_file(fp);
		pclose(fp);
		return NULL;
	}
	else {
		return fp;
	}
}

void ed_edit(char *filename, char *cmd, bool force) {
	if (!force) {
		if (state.saved == false) {
			fprintf(stderr, "Unsaved progress left\n");
			return;
		}
	}

	if (cmd != NULL) {
		FILE *fp = ed_shell(cmd, false);
		state.fromfile = false;
		state.cmd = cmd;
		state.filename = NULL;
		ll_free(gbl_head_node);
		io_load_file(fp);
		return;
	}
	else if (filename != NULL) {
		ll_free(gbl_head_node);
		state.filename = filename;
		ed_save(state.filename, NULL, 0, 0);
		io_load_file(fileopen(filename, "r"));
		return;
	}
}

node_t *ll_link_node(node_t *p, node_t *c, node_t *n) {
	c->next = n;
	c->prev = p;
	n->prev = c;
	p->next = c;
	return c;
}
	

node_t *ll_add_node(node_t *node, char *s) {
	state.saved = false;
	node_t * newnode = ll_new_node(s);

	if (node == gbl_head_node) {
		return ll_add_begin(gbl_head_node, s);
	}
	else if (node == gbl_tail_node) {
		return ll_add_end(gbl_tail_node, s);
	}
	else {
		ll_link_node(node, newnode, node->next);
		gbl_len++;
		gbl_current_ptr = ll_at_i(newnode->next);
		gbl_current_node = newnode->next;
		return gbl_current_node;
	}
}


void ed_save(char *filename, char *cmd, bool quit, bool append) {
	state.saved = true;	
	if (filename != NULL) {
		io_write_file(gbl_head_node, filename,(append)? "a":"w" );
		return;
	}
	else if (cmd != NULL) {
		FILE *fp = popen(cmd, "w");
		node_t *current = gbl_head_node;
		while (current != NULL) {
			fprintf(fp, "%s", current->s);
			current = current->next;
		}
		char *line;
		size_t linecap;
		int bytes;
		fflush(stdout);
		printf("\n");
		while ((bytes = getline(&line, &linecap, fp)) > 0) {
			printf("%s", line);
		}
		fflush(stdout);
		free(line);
		return;
	}

	if (!filename && !cmd && !quit) {
		fprintf(stderr, "Write where? Provide a filename\n");
		return;
	}	
	if (quit) {
		ed_quit(false);
	}
}

node_t * ed_copy(node_t *from, int to, node_t *at) {
	while (to > 0 && from != NULL) {
		at = ll_add_node(at, from->s);
		from = from->next;
		to--;
	}
	return at;
}

node_t * ed_move(node_t *from, int to, node_t *at) {
	ed_copy(from, to, at);
	while (to > 0 && from != NULL) {
		from = ll_remove_node(from);
		to--;
	}
	return NULL;
}

void ed_quit(bool force) {
	if (!force) {
		if (!state.saved) {
			fprintf(stderr, "Unsaved Progress\n");
			return;
		}
	}
	exit(EXIT_SUCCESS);
}


void eval(eval_t *ev) {
	char *tmp;
	switch(ev->cmd) {
		case 'a':
			ed_append(ev->from);
			break;
		case 'd':
			ed_delete(ev->from, ev->to);
			break;
		case 'c':
			ed_change(ev->from, ev->to);
			break;
		case 'e':
			if (ev->rest[0] == '!') 
				/* ed_edit(filename, cmd, force) */
				ed_edit(NULL, nextword(ev->rest), false);
			else
				ed_edit(ev->rest, NULL, false);
			break;
		case 'E':
			ed_edit(ev->rest, NULL, true);
			break;
		case 'w':
			if (ev->rest[0] == '!')
				ed_save(NULL, nextword(ev->rest), 0, 0);
			else if (ev->rest[0] == 'q')
				ed_save(state.filename, NULL, 1, 0);
			else if (isalnum(ev->rest[0]))
				ed_save(ev->rest, NULL, 0, 0);
			else
				ed_save(state.filename, NULL, 0, 0);
			break;
		case 'W':
			ed_save(ev->rest, NULL, 0, 1);
			break;
		case 'p':
			ed_print(ev->from, ev->to);
			break;
		case '!':
			ed_shell(ev->rest, true);
			break;
		case 'q':
			ed_quit(false);
			break;
		case 'Q':
			ed_quit(true);
			break;
		case 's':
			tmp = nextword(ev->rest);
			parse_address(ev, ev->rest);
			ed_subs(ev->from, ev->to, ev->regex, tmp);
			break;
		case 'k':
			ed_mark(ev->from, ev->rest[0]);
			break;
		case 'r':
			if (ev->rest[0] == '!')
				ed_read(NULL, nextword(ev->rest), ev->from);
			else
				ed_read(ev->rest, NULL, ev->from);
			break;
		case 'j':
			ed_join(ev->from, ev->to);
			break;
		default:
			printf("Unimplemented Command\n");
	}
}


/* 
 * Matches `reg` in `haystack`. Returns pointer to the match
 * and updates `matchsz` to be equal to the size of the 
 * matched substring
 * returns NULL if no match
 */
char *strreg(char *haystack, regex_t *reg, int *matchsz) {
	regmatch_t matcharr[1];	
	if (regexec(reg, haystack, 1, matcharr, 0)) {
		*matchsz = 0;
		return NULL;
	}
	*matchsz = matcharr[0].rm_eo - matcharr[0].rm_so;
	return haystack + matcharr[0].rm_so;
}


/* strncat(3) but returns pointer to the end */
char *strncata(char *dest, char *src, int n) {
	while (*dest) dest++;
	while (n > 0) {
		*dest++ = *src++;
		--n;
	}
	return dest;
}

char * regcat(char *dest, char *with, char *repstring, int *witharr) {
	while (*dest) dest++;

	int witharri = 0;
	while(*with) {
		if (*with == '&') {
			if (*(with-1) != '\\') {
				dest = strncata(dest, repstring, witharr[witharri]);
				with++;
				witharri++;
				continue;
			}
			else {
				dest--;
				*dest = '&';
			}
		}
		else {
			*dest = *with;
		}

		dest++;
		with++;
	}
	return dest;
}


/* 
 * Count the number of unescaped chars in a string 
 * also store the size of the string without unescaped
 * chars in sz
 */
int countchars(char *s, char n, int *sz) {
	int count = 0;
	*sz = 0;
	while (*s) {
		if (*s == n) { 
			if (*(s-1) != '\\') 
				count++;
		}
		else {
			*sz += 1;
		}
		s++;
	}
	return count;
}


/* Return the total sum of sizes of each substring replaced */
int rep_substr_sz(char *substr, int *substrsizes, int totalreps) {
	int size = 0;
	
	int remsize = 0;
	int namps = countchars(substr, '&', &remsize);
	for (int i = 0; i < totalreps; ++i) {
		size += (namps * substrsizes[i]) + remsize;
	}
	return size;
}



/*
 * Replace `rep` with `with` in `str`. 
 * `matchall`, if true will replace all matches in str.
 * Returns an allocated string, must be freed by the user.
 */
char *strrep(char *str, regex_t *rep, char *with, bool matchall) {
	/* Replacement happens in two passes over `str`
	 * first pass: mark what has to be replaced
	 * second pass: to replace
	 */

	int strsz = strlen(str);
	char *strstart = str;
	char *strend = str + strsz;

	/* Store pointers to substrings that will be replaced */
	char *reparr[REPLIM];
	/* Number of replacements */
	int totalreps;

	/* Store the size of each substring that will be replaced */
	int substrsizes[REPLIM];
	/* Sum of the above array */
	int repsum = 0;

	
	/* Pass 1 */
	for (int i = 0,repsz = 0; str < strend; ++i) {
		if ((str = reparr[i] = strreg(str, rep, &repsz)) == NULL) {
			totalreps = i;
			break;
		}
		str += repsz;
		totalreps = i;
		substrsizes[i] = repsz;
		repsum += substrsizes[i];

		if (!matchall)
			break;
	}

	if (totalreps == 0)
		return strstart;

	int repsubstrsz = rep_substr_sz(with, substrsizes, totalreps);

	/* retn will be the replaced string and retnsz its size */
	int retnsz = strsz + (repsubstrsz - repsum);
	char *retn = calloc(retnsz, sizeof(*retn));
	char *sretn = retn;

	/* pass 2 */
	str = strstart;
	for (int i = 0; str < strend; ) {
		if (str == reparr[i]) {
			retn = regcat(retn, with, reparr[i], substrsizes);
			str += substrsizes[i];
			++i;
			continue;
		}
		*retn= *str;
		retn++;
		str++;
	}
	free(strstart);
	return sretn;
}



// :s /do[gj]/ "mein papa" g|N
void ed_subs(node_t *from, int to, char *regex, char *rest) {
	char *flagstr;
	char *srest = rest;
	if (rest[0] == '"') {
		srest = rest = rest + 1;
		while (*rest != '"')
			rest++;
	}
	else {
		while (!isspace(*rest))
			rest++;
	}
	*rest = '\0';
	flagstr = skipspaces(rest+1);
	
	bool flag = false;
	if (*flagstr == 'g')
		flag = true;

	regex_t reg;
	int ret;
	if ((ret = regcomp(&reg, regex, REG_EXTENDED)) != 0) {
		io_reg_err(&reg, ret);
	}

	while (to > 0 && from != NULL) {
		from->s = strrep(from->s, &reg, srest, flag);
		from = from->next;
		to--;
	}
}

void ed_print(node_t *from, int to) {
	while (to > 0 && from != NULL) {
		printf("%s", from->s);
		from = from->next;
		to--;
	}
	gbl_current_node = (from) ? from : gbl_tail_node;
}

void ed_mark(node_t *node, int mark) {
	if (mark < '!' || mark > '~')
		io_err("Unacceptable or missing Mark");
	markset(node, mark);
	printf("Mark set at \"%c\"", mark);
}

void ed_read(char *filename, char *cmd, node_t *from) {
	FILE *fp;
	fp = (filename)? fileopen(filename, "r"): popen(cmd, "r");

	char *line = NULL;
	size_t linecap;
	while (getline(&line, &linecap, fp) > 0) {
		from = ll_add_node(from, line);
	}
}

char *strcata(char *dest, char *src) {
	while (*dest) dest++;
	while ((*dest++ = *src++));
	return --dest;
}

void ed_join(node_t *from, int to) {
	/* pass 1 */
	int retnarr[to];
	int retnsz = 0;
	node_t *sfrom = from;
	for (int i = 0; i < to && from != NULL; ++i, from = from->next) {
		retnarr[i] = strlen(from->s);
		retnsz += retnarr[i];
	}

	sfrom->s = realloc(sfrom->s, retnsz);
	node_t *nxt = sfrom;

	sfrom->s[retnarr[0]-1] = '\0';
	for (int i = 0; i < to && nxt != NULL; ++i) {
		strncat(sfrom->s, nxt->s, retnarr[i]-1);
		nxt = ll_remove_node(nxt);
	}
}

void io_err(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	longjmp(torepl, 1);
	return;
}

void clean() {
	ll_free(gbl_head_node);
}

void repl() {
	char *line = NULL;
	eval_t ev;
	setjmp(torepl);
	while ((line = io_read_line(EDPROMPT)) != NULL) {
		eval(parse(line, &ev));
	}
	free(line);
}

int main (/*int argc, char *argv[]*/) {
	atexit(clean);
	io_load_file(fileopen("file.txt", "r"));
	repl();	
}
