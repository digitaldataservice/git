#include "cache.h"
#include "notes.h"
#include "utf8.h"
#include "strbuf.h"
#include "tree-walk.h"

/*
 * Use a non-balancing simple 16-tree structure with struct int_node as
 * internal nodes, and struct leaf_node as leaf nodes. Each int_node has a
 * 16-array of pointers to its children.
 * The bottom 2 bits of each pointer is used to identify the pointer type
 * - ptr & 3 == 0 - NULL pointer, assert(ptr == NULL)
 * - ptr & 3 == 1 - pointer to next internal node - cast to struct int_node *
 * - ptr & 3 == 2 - pointer to note entry - cast to struct leaf_node *
 * - ptr & 3 == 3 - pointer to subtree entry - cast to struct leaf_node *
 *
 * The root node is a statically allocated struct int_node.
 */
struct int_node {
	void *a[16];
};

/*
 * Leaf nodes come in two variants, note entries and subtree entries,
 * distinguished by the LSb of the leaf node pointer (see above).
 * As a note entry, the key is the SHA1 of the referenced object, and the
 * value is the SHA1 of the note object.
 * As a subtree entry, the key is the prefix SHA1 (w/trailing NULs) of the
 * referenced object, using the last byte of the key to store the length of
 * the prefix. The value is the SHA1 of the tree object containing the notes
 * subtree.
 */
struct leaf_node {
	unsigned char key_sha1[20];
	unsigned char val_sha1[20];
};

#define PTR_TYPE_NULL     0
#define PTR_TYPE_INTERNAL 1
#define PTR_TYPE_NOTE     2
#define PTR_TYPE_SUBTREE  3

#define GET_PTR_TYPE(ptr)       ((uintptr_t) (ptr) & 3)
#define CLR_PTR_TYPE(ptr)       ((void *) ((uintptr_t) (ptr) & ~3))
#define SET_PTR_TYPE(ptr, type) ((void *) ((uintptr_t) (ptr) | (type)))

#define GET_NIBBLE(n, sha1) (((sha1[(n) >> 1]) >> ((~(n) & 0x01) << 2)) & 0x0f)

#define SUBTREE_SHA1_PREFIXCMP(key_sha1, subtree_sha1) \
	(memcmp(key_sha1, subtree_sha1, subtree_sha1[19]))

static struct int_node root_node;

static int initialized;

static void load_subtree(struct leaf_node *subtree, struct int_node *node,
		unsigned int n);

/*
 * Search the tree until the appropriate location for the given key is found:
 * 1. Start at the root node, with n = 0
 * 2. If a[0] at the current level is a matching subtree entry, unpack that
 *    subtree entry and remove it; restart search at the current level.
 * 3. Use the nth nibble of the key as an index into a:
 *    - If a[n] is an int_node, recurse from #2 into that node and increment n
 *    - If a matching subtree entry, unpack that subtree entry (and remove it);
 *      restart search at the current level.
 *    - Otherwise, we have found one of the following:
 *      - a subtree entry which does not match the key
 *      - a note entry which may or may not match the key
 *      - an unused leaf node (NULL)
 *      In any case, set *tree and *n, and return pointer to the tree location.
 */
static void **note_tree_search(struct int_node **tree,
		unsigned char *n, const unsigned char *key_sha1)
{
	struct leaf_node *l;
	unsigned char i;
	void *p = (*tree)->a[0];

	if (GET_PTR_TYPE(p) == PTR_TYPE_SUBTREE) {
		l = (struct leaf_node *) CLR_PTR_TYPE(p);
		if (!SUBTREE_SHA1_PREFIXCMP(key_sha1, l->key_sha1)) {
			/* unpack tree and resume search */
			(*tree)->a[0] = NULL;
			load_subtree(l, *tree, *n);
			free(l);
			return note_tree_search(tree, n, key_sha1);
		}
	}

	i = GET_NIBBLE(*n, key_sha1);
	p = (*tree)->a[i];
	switch (GET_PTR_TYPE(p)) {
	case PTR_TYPE_INTERNAL:
		*tree = CLR_PTR_TYPE(p);
		(*n)++;
		return note_tree_search(tree, n, key_sha1);
	case PTR_TYPE_SUBTREE:
		l = (struct leaf_node *) CLR_PTR_TYPE(p);
		if (!SUBTREE_SHA1_PREFIXCMP(key_sha1, l->key_sha1)) {
			/* unpack tree and resume search */
			(*tree)->a[i] = NULL;
			load_subtree(l, *tree, *n);
			free(l);
			return note_tree_search(tree, n, key_sha1);
		}
		/* fall through */
	default:
		return &((*tree)->a[i]);
	}
}

/*
 * To find a leaf_node:
 * Search to the tree location appropriate for the given key:
 * If a note entry with matching key, return the note entry, else return NULL.
 */
static struct leaf_node *note_tree_find(struct int_node *tree, unsigned char n,
		const unsigned char *key_sha1)
{
	void **p = note_tree_search(&tree, &n, key_sha1);
	if (GET_PTR_TYPE(*p) == PTR_TYPE_NOTE) {
		struct leaf_node *l = (struct leaf_node *) CLR_PTR_TYPE(*p);
		if (!hashcmp(key_sha1, l->key_sha1))
			return l;
	}
	return NULL;
}

/* Create a new blob object by concatenating the two given blob objects */
static int concatenate_notes(unsigned char *cur_sha1,
		const unsigned char *new_sha1)
{
	char *cur_msg, *new_msg, *buf;
	unsigned long cur_len, new_len, buf_len;
	enum object_type cur_type, new_type;
	int ret;

	/* read in both note blob objects */
	new_msg = read_sha1_file(new_sha1, &new_type, &new_len);
	if (!new_msg || !new_len || new_type != OBJ_BLOB) {
		free(new_msg);
		return 0;
	}
	cur_msg = read_sha1_file(cur_sha1, &cur_type, &cur_len);
	if (!cur_msg || !cur_len || cur_type != OBJ_BLOB) {
		free(cur_msg);
		free(new_msg);
		hashcpy(cur_sha1, new_sha1);
		return 0;
	}

	/* we will separate the notes by a newline anyway */
	if (cur_msg[cur_len - 1] == '\n')
		cur_len--;

	/* concatenate cur_msg and new_msg into buf */
	buf_len = cur_len + 1 + new_len;
	buf = (char *) xmalloc(buf_len);
	memcpy(buf, cur_msg, cur_len);
	buf[cur_len] = '\n';
	memcpy(buf + cur_len + 1, new_msg, new_len);

	free(cur_msg);
	free(new_msg);

	/* create a new blob object from buf */
	ret = write_sha1_file(buf, buf_len, "blob", cur_sha1);
	free(buf);
	return ret;
}

/*
 * To insert a leaf_node:
 * Search to the tree location appropriate for the given leaf_node's key:
 * - If location is unused (NULL), store the tweaked pointer directly there
 * - If location holds a note entry that matches the note-to-be-inserted, then
 *   concatenate the two notes.
 * - If location holds a note entry that matches the subtree-to-be-inserted,
 *   then unpack the subtree-to-be-inserted into the location.
 * - If location holds a matching subtree entry, unpack the subtree at that
 *   location, and restart the insert operation from that level.
 * - Else, create a new int_node, holding both the node-at-location and the
 *   node-to-be-inserted, and store the new int_node into the location.
 */
static void note_tree_insert(struct int_node *tree, unsigned char n,
		struct leaf_node *entry, unsigned char type)
{
	struct int_node *new_node;
	struct leaf_node *l;
	void **p = note_tree_search(&tree, &n, entry->key_sha1);

	assert(GET_PTR_TYPE(entry) == 0); /* no type bits set */
	l = (struct leaf_node *) CLR_PTR_TYPE(*p);
	switch (GET_PTR_TYPE(*p)) {
	case PTR_TYPE_NULL:
		assert(!*p);
		*p = SET_PTR_TYPE(entry, type);
		return;
	case PTR_TYPE_NOTE:
		switch (type) {
		case PTR_TYPE_NOTE:
			if (!hashcmp(l->key_sha1, entry->key_sha1)) {
				/* skip concatenation if l == entry */
				if (!hashcmp(l->val_sha1, entry->val_sha1))
					return;

				if (concatenate_notes(l->val_sha1,
						entry->val_sha1))
					die("failed to concatenate note %s "
					    "into note %s for object %s",
					    sha1_to_hex(entry->val_sha1),
					    sha1_to_hex(l->val_sha1),
					    sha1_to_hex(l->key_sha1));
				free(entry);
				return;
			}
			break;
		case PTR_TYPE_SUBTREE:
			if (!SUBTREE_SHA1_PREFIXCMP(l->key_sha1,
						    entry->key_sha1)) {
				/* unpack 'entry' */
				load_subtree(entry, tree, n);
				free(entry);
				return;
			}
			break;
		}
		break;
	case PTR_TYPE_SUBTREE:
		if (!SUBTREE_SHA1_PREFIXCMP(entry->key_sha1, l->key_sha1)) {
			/* unpack 'l' and restart insert */
			*p = NULL;
			load_subtree(l, tree, n);
			free(l);
			note_tree_insert(tree, n, entry, type);
			return;
		}
		break;
	}

	/* non-matching leaf_node */
	assert(GET_PTR_TYPE(*p) == PTR_TYPE_NOTE ||
	       GET_PTR_TYPE(*p) == PTR_TYPE_SUBTREE);
	new_node = (struct int_node *) xcalloc(sizeof(struct int_node), 1);
	note_tree_insert(new_node, n + 1, l, GET_PTR_TYPE(*p));
	*p = SET_PTR_TYPE(new_node, PTR_TYPE_INTERNAL);
	note_tree_insert(new_node, n + 1, entry, type);
}

/*
 * How to consolidate an int_node:
 * If there are > 1 non-NULL entries, give up and return non-zero.
 * Otherwise replace the int_node at the given index in the given parent node
 * with the only entry (or a NULL entry if no entries) from the given tree,
 * and return 0.
 */
static int note_tree_consolidate(struct int_node *tree,
	struct int_node *parent, unsigned char index)
{
	unsigned int i;
	void *p = NULL;

	assert(tree && parent);
	assert(CLR_PTR_TYPE(parent->a[index]) == tree);

	for (i = 0; i < 16; i++) {
		if (GET_PTR_TYPE(tree->a[i]) != PTR_TYPE_NULL) {
			if (p) /* more than one entry */
				return -2;
			p = tree->a[i];
		}
	}

	/* replace tree with p in parent[index] */
	parent->a[index] = p;
	free(tree);
	return 0;
}

/*
 * To remove a leaf_node:
 * Search to the tree location appropriate for the given leaf_node's key:
 * - If location does not hold a matching entry, abort and do nothing.
 * - Replace the matching leaf_node with a NULL entry (and free the leaf_node).
 * - Consolidate int_nodes repeatedly, while walking up the tree towards root.
 */
static void note_tree_remove(struct int_node *tree, unsigned char n,
		struct leaf_node *entry)
{
	struct leaf_node *l;
	struct int_node *parent_stack[20];
	unsigned char i, j;
	void **p = note_tree_search(&tree, &n, entry->key_sha1);

	assert(GET_PTR_TYPE(entry) == 0); /* no type bits set */
	if (GET_PTR_TYPE(*p) != PTR_TYPE_NOTE)
		return; /* type mismatch, nothing to remove */
	l = (struct leaf_node *) CLR_PTR_TYPE(*p);
	if (hashcmp(l->key_sha1, entry->key_sha1))
		return; /* key mismatch, nothing to remove */

	/* we have found a matching entry */
	free(l);
	*p = SET_PTR_TYPE(NULL, PTR_TYPE_NULL);

	/* consolidate this tree level, and parent levels, if possible */
	if (!n)
		return; /* cannot consolidate top level */
	/* first, build stack of ancestors between root and current node */
	parent_stack[0] = &root_node;
	for (i = 0; i < n; i++) {
		j = GET_NIBBLE(i, entry->key_sha1);
		parent_stack[i + 1] = CLR_PTR_TYPE(parent_stack[i]->a[j]);
	}
	assert(i == n && parent_stack[i] == tree);
	/* next, unwind stack until note_tree_consolidate() is done */
	while (i > 0 &&
	       !note_tree_consolidate(parent_stack[i], parent_stack[i - 1],
				      GET_NIBBLE(i - 1, entry->key_sha1)))
		i--;
}

/* Free the entire notes data contained in the given tree */
static void note_tree_free(struct int_node *tree)
{
	unsigned int i;
	for (i = 0; i < 16; i++) {
		void *p = tree->a[i];
		switch (GET_PTR_TYPE(p)) {
		case PTR_TYPE_INTERNAL:
			note_tree_free(CLR_PTR_TYPE(p));
			/* fall through */
		case PTR_TYPE_NOTE:
		case PTR_TYPE_SUBTREE:
			free(CLR_PTR_TYPE(p));
		}
	}
}

/*
 * Convert a partial SHA1 hex string to the corresponding partial SHA1 value.
 * - hex      - Partial SHA1 segment in ASCII hex format
 * - hex_len  - Length of above segment. Must be multiple of 2 between 0 and 40
 * - sha1     - Partial SHA1 value is written here
 * - sha1_len - Max #bytes to store in sha1, Must be >= hex_len / 2, and < 20
 * Returns -1 on error (invalid arguments or invalid SHA1 (not in hex format)).
 * Otherwise, returns number of bytes written to sha1 (i.e. hex_len / 2).
 * Pads sha1 with NULs up to sha1_len (not included in returned length).
 */
static int get_sha1_hex_segment(const char *hex, unsigned int hex_len,
		unsigned char *sha1, unsigned int sha1_len)
{
	unsigned int i, len = hex_len >> 1;
	if (hex_len % 2 != 0 || len > sha1_len)
		return -1;
	for (i = 0; i < len; i++) {
		unsigned int val = (hexval(hex[0]) << 4) | hexval(hex[1]);
		if (val & ~0xff)
			return -1;
		*sha1++ = val;
		hex += 2;
	}
	for (; i < sha1_len; i++)
		*sha1++ = 0;
	return len;
}

static void load_subtree(struct leaf_node *subtree, struct int_node *node,
		unsigned int n)
{
	unsigned char object_sha1[20];
	unsigned int prefix_len;
	void *buf;
	struct tree_desc desc;
	struct name_entry entry;

	buf = fill_tree_descriptor(&desc, subtree->val_sha1);
	if (!buf)
		die("Could not read %s for notes-index",
		     sha1_to_hex(subtree->val_sha1));

	prefix_len = subtree->key_sha1[19];
	assert(prefix_len * 2 >= n);
	memcpy(object_sha1, subtree->key_sha1, prefix_len);
	while (tree_entry(&desc, &entry)) {
		int len = get_sha1_hex_segment(entry.path, strlen(entry.path),
				object_sha1 + prefix_len, 20 - prefix_len);
		if (len < 0)
			continue; /* entry.path is not a SHA1 sum. Skip */
		len += prefix_len;

		/*
		 * If object SHA1 is complete (len == 20), assume note object
		 * If object SHA1 is incomplete (len < 20), assume note subtree
		 */
		if (len <= 20) {
			unsigned char type = PTR_TYPE_NOTE;
			struct leaf_node *l = (struct leaf_node *)
				xcalloc(sizeof(struct leaf_node), 1);
			hashcpy(l->key_sha1, object_sha1);
			hashcpy(l->val_sha1, entry.sha1);
			if (len < 20) {
				if (!S_ISDIR(entry.mode))
					continue; /* entry cannot be subtree */
				l->key_sha1[19] = (unsigned char) len;
				type = PTR_TYPE_SUBTREE;
			}
			note_tree_insert(node, n, l, type);
		}
	}
	free(buf);
}

/*
 * Determine optimal on-disk fanout for this part of the notes tree
 *
 * Given a (sub)tree and the level in the internal tree structure, determine
 * whether or not the given existing fanout should be expanded for this
 * (sub)tree.
 *
 * Values of the 'fanout' variable:
 * - 0: No fanout (all notes are stored directly in the root notes tree)
 * - 1: 2/38 fanout
 * - 2: 2/2/36 fanout
 * - 3: 2/2/2/34 fanout
 * etc.
 */
static unsigned char determine_fanout(struct int_node *tree, unsigned char n,
		unsigned char fanout)
{
	/*
	 * The following is a simple heuristic that works well in practice:
	 * For each even-numbered 16-tree level (remember that each on-disk
	 * fanout level corresponds to _two_ 16-tree levels), peek at all 16
	 * entries at that tree level. If all of them are either int_nodes or
	 * subtree entries, then there are likely plenty of notes below this
	 * level, so we return an incremented fanout.
	 */
	unsigned int i;
	if ((n % 2) || (n > 2 * fanout))
		return fanout;
	for (i = 0; i < 16; i++) {
		switch (GET_PTR_TYPE(tree->a[i])) {
		case PTR_TYPE_SUBTREE:
		case PTR_TYPE_INTERNAL:
			continue;
		default:
			return fanout;
		}
	}
	return fanout + 1;
}

static void construct_path_with_fanout(const unsigned char *sha1,
		unsigned char fanout, char *path)
{
	unsigned int i = 0, j = 0;
	const char *hex_sha1 = sha1_to_hex(sha1);
	assert(fanout < 20);
	while (fanout) {
		path[i++] = hex_sha1[j++];
		path[i++] = hex_sha1[j++];
		path[i++] = '/';
		fanout--;
	}
	strcpy(path + i, hex_sha1 + j);
}

static int for_each_note_helper(struct int_node *tree, unsigned char n,
		unsigned char fanout, int flags, each_note_fn fn,
		void *cb_data)
{
	unsigned int i;
	void *p;
	int ret = 0;
	struct leaf_node *l;
	static char path[40 + 19 + 1];  /* hex SHA1 + 19 * '/' + NUL */

	fanout = determine_fanout(tree, n, fanout);
	for (i = 0; i < 16; i++) {
redo:
		p = tree->a[i];
		switch (GET_PTR_TYPE(p)) {
		case PTR_TYPE_INTERNAL:
			/* recurse into int_node */
			ret = for_each_note_helper(CLR_PTR_TYPE(p), n + 1,
				fanout, flags, fn, cb_data);
			break;
		case PTR_TYPE_SUBTREE:
			l = (struct leaf_node *) CLR_PTR_TYPE(p);
			/*
			 * Subtree entries in the note tree represent parts of
			 * the note tree that have not yet been explored. There
			 * is a direct relationship between subtree entries at
			 * level 'n' in the tree, and the 'fanout' variable:
			 * Subtree entries at level 'n <= 2 * fanout' should be
			 * preserved, since they correspond exactly to a fanout
			 * directory in the on-disk structure. However, subtree
			 * entries at level 'n > 2 * fanout' should NOT be
			 * preserved, but rather consolidated into the above
			 * notes tree level. We achieve this by unconditionally
			 * unpacking subtree entries that exist below the
			 * threshold level at 'n = 2 * fanout'.
			 */
			if (n <= 2 * fanout &&
			    flags & FOR_EACH_NOTE_YIELD_SUBTREES) {
				/* invoke callback with subtree */
				unsigned int path_len =
					l->key_sha1[19] * 2 + fanout;
				assert(path_len < 40 + 19);
				construct_path_with_fanout(l->key_sha1, fanout,
							   path);
				/* Create trailing slash, if needed */
				if (path[path_len - 1] != '/')
					path[path_len++] = '/';
				path[path_len] = '\0';
				ret = fn(l->key_sha1, l->val_sha1, path,
					 cb_data);
			}
			if (n > fanout * 2 ||
			    !(flags & FOR_EACH_NOTE_DONT_UNPACK_SUBTREES)) {
				/* unpack subtree and resume traversal */
				tree->a[i] = NULL;
				load_subtree(l, tree, n);
				free(l);
				goto redo;
			}
			break;
		case PTR_TYPE_NOTE:
			l = (struct leaf_node *) CLR_PTR_TYPE(p);
			construct_path_with_fanout(l->key_sha1, fanout, path);
			ret = fn(l->key_sha1, l->val_sha1, path, cb_data);
			break;
		}
		if (ret)
			return ret;
	}
	return 0;
}

void init_notes(const char *notes_ref, int flags)
{
	unsigned char sha1[20], object_sha1[20];
	unsigned mode;
	struct leaf_node root_tree;

	assert(!initialized);
	initialized = 1;

	if (!notes_ref)
		notes_ref = getenv(GIT_NOTES_REF_ENVIRONMENT);
	if (!notes_ref)
		notes_ref = notes_ref_name; /* value of core.notesRef config */
	if (!notes_ref)
		notes_ref = GIT_NOTES_DEFAULT_REF;

	if (flags & NOTES_INIT_EMPTY || !notes_ref ||
	    read_ref(notes_ref, object_sha1))
		return;
	if (get_tree_entry(object_sha1, "", sha1, &mode))
		die("Failed to read notes tree referenced by %s (%s)",
		    notes_ref, object_sha1);

	hashclr(root_tree.key_sha1);
	hashcpy(root_tree.val_sha1, sha1);
	load_subtree(&root_tree, &root_node, 0);
}

void add_note(const unsigned char *object_sha1, const unsigned char *note_sha1)
{
	struct leaf_node *l;

	assert(initialized);
	l = (struct leaf_node *) xmalloc(sizeof(struct leaf_node));
	hashcpy(l->key_sha1, object_sha1);
	hashcpy(l->val_sha1, note_sha1);
	note_tree_insert(&root_node, 0, l, PTR_TYPE_NOTE);
}

void remove_note(const unsigned char *object_sha1)
{
	struct leaf_node l;

	assert(initialized);
	hashcpy(l.key_sha1, object_sha1);
	hashclr(l.val_sha1);
	return note_tree_remove(&root_node, 0, &l);
}

const unsigned char *get_note(const unsigned char *object_sha1)
{
	struct leaf_node *found;

	assert(initialized);
	found = note_tree_find(&root_node, 0, object_sha1);
	return found ? found->val_sha1 : NULL;
}

int for_each_note(int flags, each_note_fn fn, void *cb_data)
{
	assert(initialized);
	return for_each_note_helper(&root_node, 0, 0, flags, fn, cb_data);
}

void free_notes(void)
{
	note_tree_free(&root_node);
	memset(&root_node, 0, sizeof(struct int_node));
	initialized = 0;
}

void format_note(const unsigned char *object_sha1, struct strbuf *sb,
		const char *output_encoding, int flags)
{
	static const char utf8[] = "utf-8";
	const unsigned char *sha1;
	char *msg, *msg_p;
	unsigned long linelen, msglen;
	enum object_type type;

	if (!initialized)
		init_notes(NULL, 0);

	sha1 = get_note(object_sha1);
	if (!sha1)
		return;

	if (!(msg = read_sha1_file(sha1, &type, &msglen)) || !msglen ||
			type != OBJ_BLOB) {
		free(msg);
		return;
	}

	if (output_encoding && *output_encoding &&
			strcmp(utf8, output_encoding)) {
		char *reencoded = reencode_string(msg, output_encoding, utf8);
		if (reencoded) {
			free(msg);
			msg = reencoded;
			msglen = strlen(msg);
		}
	}

	/* we will end the annotation by a newline anyway */
	if (msglen && msg[msglen - 1] == '\n')
		msglen--;

	if (flags & NOTES_SHOW_HEADER)
		strbuf_addstr(sb, "\nNotes:\n");

	for (msg_p = msg; msg_p < msg + msglen; msg_p += linelen + 1) {
		linelen = strchrnul(msg_p, '\n') - msg_p;

		if (flags & NOTES_INDENT)
			strbuf_addstr(sb, "    ");
		strbuf_add(sb, msg_p, linelen);
		strbuf_addch(sb, '\n');
	}

	free(msg);
}
