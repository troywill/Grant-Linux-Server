#include "cache.h"
#include "tag.h"
#include "commit.h"
#include "tree.h"
#include "blob.h"

const char *tag_type = "tag";

struct object *deref_tag(struct object *o, const char *warn, int warnlen)
{
	while (o && o->type == OBJ_TAG)
		if (((struct tag *)o)->tagged)
			o = parse_object(((struct tag *)o)->tagged->sha1);
		else
			o = NULL;
	if (!o && warn) {
		if (!warnlen)
			warnlen = strlen(warn);
		error("missing object referenced by '%.*s'", warnlen, warn);
	}
	return o;
}

struct tag *lookup_tag(const unsigned char *sha1)
{
	struct object *obj = lookup_object(sha1);
	if (!obj)
		return create_object(sha1, OBJ_TAG, alloc_tag_node());
	if (!obj->type)
		obj->type = OBJ_TAG;
        if (obj->type != OBJ_TAG) {
                error("Object %s is a %s, not a tag",
                      sha1_to_hex(sha1), typename(obj->type));
                return NULL;
        }
        return (struct tag *) obj;
}

int parse_tag_buffer(struct tag *item, void *data, unsigned long size)
{
	int typelen, taglen;
	unsigned char sha1[20];
	const char *type_line, *tag_line, *sig_line;
	char type[20];
	const char *start = data;

        if (item->object.parsed)
                return 0;
        item->object.parsed = 1;

	if (size < 64)
		return -1;
	if (memcmp("object ", data, 7) || get_sha1_hex((char *) data + 7, sha1))
		return -1;

	type_line = (char *) data + 48;
	if (memcmp("\ntype ", type_line-1, 6))
		return -1;

	tag_line = memchr(type_line, '\n', size - (type_line - start));
	if (!tag_line || memcmp("tag ", ++tag_line, 4))
		return -1;

	sig_line = memchr(tag_line, '\n', size - (tag_line - start));
	if (!sig_line)
		return -1;
	sig_line++;

	typelen = tag_line - type_line - strlen("type \n");
	if (typelen >= 20)
		return -1;
	memcpy(type, type_line + 5, typelen);
	type[typelen] = '\0';
	taglen = sig_line - tag_line - strlen("tag \n");
	item->tag = xmemdupz(tag_line + 4, taglen);

	if (!strcmp(type, blob_type)) {
		item->tagged = &lookup_blob(sha1)->object;
	} else if (!strcmp(type, tree_type)) {
		item->tagged = &lookup_tree(sha1)->object;
	} else if (!strcmp(type, commit_type)) {
		item->tagged = &lookup_commit(sha1)->object;
	} else if (!strcmp(type, tag_type)) {
		item->tagged = &lookup_tag(sha1)->object;
	} else {
		error("Unknown type %s", type);
		item->tagged = NULL;
	}

	return 0;
}

int parse_tag(struct tag *item)
{
	enum object_type type;
	void *data;
	unsigned long size;
	int ret;

	if (item->object.parsed)
		return 0;
	data = read_sha1_file(item->object.sha1, &type, &size);
	if (!data)
		return error("Could not read %s",
			     sha1_to_hex(item->object.sha1));
	if (type != OBJ_TAG) {
		free(data);
		return error("Object %s not a tag",
			     sha1_to_hex(item->object.sha1));
	}
	ret = parse_tag_buffer(item, data, size);
	free(data);
	return ret;
}
