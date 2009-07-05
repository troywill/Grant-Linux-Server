#include "cache.h"
#include "commit.h"
#include "pack.h"
#include "tag.h"
#include "blob.h"
#include "http.h"
#include "refs.h"
#include "diff.h"
#include "revision.h"
#include "exec_cmd.h"
#include "remote.h"
#include "list-objects.h"
#include "sigchain.h"

#include <expat.h>

static const char http_push_usage[] =
"git http-push [--all] [--dry-run] [--force] [--verbose] <remote> [<head>...]\n";

#ifndef XML_STATUS_OK
enum XML_Status {
  XML_STATUS_OK = 1,
  XML_STATUS_ERROR = 0
};
#define XML_STATUS_OK    1
#define XML_STATUS_ERROR 0
#endif

#define PREV_BUF_SIZE 4096
#define RANGE_HEADER_SIZE 30

/* DAV methods */
#define DAV_LOCK "LOCK"
#define DAV_MKCOL "MKCOL"
#define DAV_MOVE "MOVE"
#define DAV_PROPFIND "PROPFIND"
#define DAV_PUT "PUT"
#define DAV_UNLOCK "UNLOCK"
#define DAV_DELETE "DELETE"

/* DAV lock flags */
#define DAV_PROP_LOCKWR (1u << 0)
#define DAV_PROP_LOCKEX (1u << 1)
#define DAV_LOCK_OK (1u << 2)

/* DAV XML properties */
#define DAV_CTX_LOCKENTRY ".multistatus.response.propstat.prop.supportedlock.lockentry"
#define DAV_CTX_LOCKTYPE_WRITE ".multistatus.response.propstat.prop.supportedlock.lockentry.locktype.write"
#define DAV_CTX_LOCKTYPE_EXCLUSIVE ".multistatus.response.propstat.prop.supportedlock.lockentry.lockscope.exclusive"
#define DAV_ACTIVELOCK_OWNER ".prop.lockdiscovery.activelock.owner.href"
#define DAV_ACTIVELOCK_TIMEOUT ".prop.lockdiscovery.activelock.timeout"
#define DAV_ACTIVELOCK_TOKEN ".prop.lockdiscovery.activelock.locktoken.href"
#define DAV_PROPFIND_RESP ".multistatus.response"
#define DAV_PROPFIND_NAME ".multistatus.response.href"
#define DAV_PROPFIND_COLLECTION ".multistatus.response.propstat.prop.resourcetype.collection"

/* DAV request body templates */
#define PROPFIND_SUPPORTEDLOCK_REQUEST "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n<D:propfind xmlns:D=\"DAV:\">\n<D:prop xmlns:R=\"%s\">\n<D:supportedlock/>\n</D:prop>\n</D:propfind>"
#define PROPFIND_ALL_REQUEST "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n<D:propfind xmlns:D=\"DAV:\">\n<D:allprop/>\n</D:propfind>"
#define LOCK_REQUEST "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n<D:lockinfo xmlns:D=\"DAV:\">\n<D:lockscope><D:exclusive/></D:lockscope>\n<D:locktype><D:write/></D:locktype>\n<D:owner>\n<D:href>mailto:%s</D:href>\n</D:owner>\n</D:lockinfo>"

#define LOCK_TIME 600
#define LOCK_REFRESH 30

/* bits #0-15 in revision.h */

#define LOCAL    (1u<<16)
#define REMOTE   (1u<<17)
#define FETCHING (1u<<18)
#define PUSHING  (1u<<19)

/* We allow "recursive" symbolic refs. Only within reason, though */
#define MAXDEPTH 5

static int pushing;
static int aborted;
static signed char remote_dir_exists[256];

static struct curl_slist *no_pragma_header;

static int push_verbosely;
static int push_all = MATCH_REFS_NONE;
static int force_all;
static int dry_run;

static struct object_list *objects;

struct repo
{
	char *url;
	char *path;
	int path_len;
	int has_info_refs;
	int can_update_info_refs;
	int has_info_packs;
	struct packed_git *packs;
	struct remote_lock *locks;
};

static struct repo *repo;

enum transfer_state {
	NEED_FETCH,
	RUN_FETCH_LOOSE,
	RUN_FETCH_PACKED,
	NEED_PUSH,
	RUN_MKCOL,
	RUN_PUT,
	RUN_MOVE,
	ABORTED,
	COMPLETE,
};

struct transfer_request
{
	struct object *obj;
	char *url;
	char *dest;
	struct remote_lock *lock;
	struct curl_slist *headers;
	struct buffer buffer;
	char filename[PATH_MAX];
	char tmpfile[PATH_MAX];
	int local_fileno;
	FILE *local_stream;
	enum transfer_state state;
	CURLcode curl_result;
	char errorstr[CURL_ERROR_SIZE];
	long http_code;
	unsigned char real_sha1[20];
	git_SHA_CTX c;
	z_stream stream;
	int zret;
	int rename;
	void *userData;
	struct active_request_slot *slot;
	struct transfer_request *next;
};

static struct transfer_request *request_queue_head;

struct xml_ctx
{
	char *name;
	int len;
	char *cdata;
	void (*userFunc)(struct xml_ctx *ctx, int tag_closed);
	void *userData;
};

struct remote_lock
{
	char *url;
	char *owner;
	char *token;
	char tmpfile_suffix[41];
	time_t start_time;
	long timeout;
	int refreshing;
	struct remote_lock *next;
};

/* Flags that control remote_ls processing */
#define PROCESS_FILES (1u << 0)
#define PROCESS_DIRS  (1u << 1)
#define RECURSIVE     (1u << 2)

/* Flags that remote_ls passes to callback functions */
#define IS_DIR (1u << 0)

struct remote_ls_ctx
{
	char *path;
	void (*userFunc)(struct remote_ls_ctx *ls);
	void *userData;
	int flags;
	char *dentry_name;
	int dentry_flags;
	struct remote_ls_ctx *parent;
};

/* get_dav_token_headers options */
enum dav_header_flag {
	DAV_HEADER_IF = (1u << 0),
	DAV_HEADER_LOCK = (1u << 1),
	DAV_HEADER_TIMEOUT = (1u << 2)
};

static char *xml_entities(char *s)
{
	struct strbuf buf = STRBUF_INIT;
	while (*s) {
		size_t len = strcspn(s, "\"<>&");
		strbuf_add(&buf, s, len);
		s += len;
		switch (*s) {
		case '"':
			strbuf_addstr(&buf, "&quot;");
			break;
		case '<':
			strbuf_addstr(&buf, "&lt;");
			break;
		case '>':
			strbuf_addstr(&buf, "&gt;");
			break;
		case '&':
			strbuf_addstr(&buf, "&amp;");
			break;
		}
		s++;
	}
	return strbuf_detach(&buf, NULL);
}

static struct curl_slist *get_dav_token_headers(struct remote_lock *lock, enum dav_header_flag options)
{
	struct strbuf buf = STRBUF_INIT;
	struct curl_slist *dav_headers = NULL;

	if (options & DAV_HEADER_IF) {
		strbuf_addf(&buf, "If: (<%s>)", lock->token);
		dav_headers = curl_slist_append(dav_headers, buf.buf);
		strbuf_reset(&buf);
	}
	if (options & DAV_HEADER_LOCK) {
		strbuf_addf(&buf, "Lock-Token: <%s>", lock->token);
		dav_headers = curl_slist_append(dav_headers, buf.buf);
		strbuf_reset(&buf);
	}
	if (options & DAV_HEADER_TIMEOUT) {
		strbuf_addf(&buf, "Timeout: Second-%ld", lock->timeout);
		dav_headers = curl_slist_append(dav_headers, buf.buf);
		strbuf_reset(&buf);
	}
	strbuf_release(&buf);

	return dav_headers;
}

static void append_remote_object_url(struct strbuf *buf, const char *url,
				     const char *hex,
				     int only_two_digit_prefix)
{
	strbuf_addf(buf, "%sobjects/%.*s/", url, 2, hex);
	if (!only_two_digit_prefix)
		strbuf_addf(buf, "%s", hex+2);
}

static void finish_request(struct transfer_request *request);
static void release_request(struct transfer_request *request);

static void process_response(void *callback_data)
{
	struct transfer_request *request =
		(struct transfer_request *)callback_data;

	finish_request(request);
}

#ifdef USE_CURL_MULTI

static char *get_remote_object_url(const char *url, const char *hex,
				   int only_two_digit_prefix)
{
	struct strbuf buf = STRBUF_INIT;
	append_remote_object_url(&buf, url, hex, only_two_digit_prefix);
	return strbuf_detach(&buf, NULL);
}

static size_t fwrite_sha1_file(void *ptr, size_t eltsize, size_t nmemb,
			       void *data)
{
	unsigned char expn[4096];
	size_t size = eltsize * nmemb;
	int posn = 0;
	struct transfer_request *request = (struct transfer_request *)data;
	do {
		ssize_t retval = xwrite(request->local_fileno,
				       (char *) ptr + posn, size - posn);
		if (retval < 0)
			return posn;
		posn += retval;
	} while (posn < size);

	request->stream.avail_in = size;
	request->stream.next_in = ptr;
	do {
		request->stream.next_out = expn;
		request->stream.avail_out = sizeof(expn);
		request->zret = git_inflate(&request->stream, Z_SYNC_FLUSH);
		git_SHA1_Update(&request->c, expn,
			    sizeof(expn) - request->stream.avail_out);
	} while (request->stream.avail_in && request->zret == Z_OK);
	data_received++;
	return size;
}

static void start_fetch_loose(struct transfer_request *request)
{
	char *hex = sha1_to_hex(request->obj->sha1);
	char *filename;
	char prevfile[PATH_MAX];
	char *url;
	int prevlocal;
	unsigned char prev_buf[PREV_BUF_SIZE];
	ssize_t prev_read = 0;
	long prev_posn = 0;
	char range[RANGE_HEADER_SIZE];
	struct curl_slist *range_header = NULL;
	struct active_request_slot *slot;

	filename = sha1_file_name(request->obj->sha1);
	snprintf(request->filename, sizeof(request->filename), "%s", filename);
	snprintf(request->tmpfile, sizeof(request->tmpfile),
		 "%s.temp", filename);

	snprintf(prevfile, sizeof(prevfile), "%s.prev", request->filename);
	unlink_or_warn(prevfile);
	rename(request->tmpfile, prevfile);
	unlink_or_warn(request->tmpfile);

	if (request->local_fileno != -1)
		error("fd leakage in start: %d", request->local_fileno);
	request->local_fileno = open(request->tmpfile,
				     O_WRONLY | O_CREAT | O_EXCL, 0666);
	/* This could have failed due to the "lazy directory creation";
	 * try to mkdir the last path component.
	 */
	if (request->local_fileno < 0 && errno == ENOENT) {
		char *dir = strrchr(request->tmpfile, '/');
		if (dir) {
			*dir = 0;
			mkdir(request->tmpfile, 0777);
			*dir = '/';
		}
		request->local_fileno = open(request->tmpfile,
					     O_WRONLY | O_CREAT | O_EXCL, 0666);
	}

	if (request->local_fileno < 0) {
		request->state = ABORTED;
		error("Couldn't create temporary file %s for %s: %s",
		      request->tmpfile, request->filename, strerror(errno));
		return;
	}

	memset(&request->stream, 0, sizeof(request->stream));

	git_inflate_init(&request->stream);

	git_SHA1_Init(&request->c);

	url = get_remote_object_url(repo->url, hex, 0);
	request->url = xstrdup(url);

	/* If a previous temp file is present, process what was already
	   fetched. */
	prevlocal = open(prevfile, O_RDONLY);
	if (prevlocal != -1) {
		do {
			prev_read = xread(prevlocal, prev_buf, PREV_BUF_SIZE);
			if (prev_read>0) {
				if (fwrite_sha1_file(prev_buf,
						     1,
						     prev_read,
						     request) == prev_read) {
					prev_posn += prev_read;
				} else {
					prev_read = -1;
				}
			}
		} while (prev_read > 0);
		close(prevlocal);
	}
	unlink_or_warn(prevfile);

	/* Reset inflate/SHA1 if there was an error reading the previous temp
	   file; also rewind to the beginning of the local file. */
	if (prev_read == -1) {
		memset(&request->stream, 0, sizeof(request->stream));
		git_inflate_init(&request->stream);
		git_SHA1_Init(&request->c);
		if (prev_posn>0) {
			prev_posn = 0;
			lseek(request->local_fileno, 0, SEEK_SET);
			ftruncate(request->local_fileno, 0);
		}
	}

	slot = get_active_slot();
	slot->callback_func = process_response;
	slot->callback_data = request;
	request->slot = slot;

	curl_easy_setopt(slot->curl, CURLOPT_FILE, request);
	curl_easy_setopt(slot->curl, CURLOPT_WRITEFUNCTION, fwrite_sha1_file);
	curl_easy_setopt(slot->curl, CURLOPT_ERRORBUFFER, request->errorstr);
	curl_easy_setopt(slot->curl, CURLOPT_URL, url);
	curl_easy_setopt(slot->curl, CURLOPT_HTTPHEADER, no_pragma_header);

	/* If we have successfully processed data from a previous fetch
	   attempt, only fetch the data we don't already have. */
	if (prev_posn>0) {
		if (push_verbosely)
			fprintf(stderr,
				"Resuming fetch of object %s at byte %ld\n",
				hex, prev_posn);
		sprintf(range, "Range: bytes=%ld-", prev_posn);
		range_header = curl_slist_append(range_header, range);
		curl_easy_setopt(slot->curl,
				 CURLOPT_HTTPHEADER, range_header);
	}

	/* Try to get the request started, abort the request on error */
	request->state = RUN_FETCH_LOOSE;
	if (!start_active_slot(slot)) {
		fprintf(stderr, "Unable to start GET request\n");
		repo->can_update_info_refs = 0;
		release_request(request);
	}
}

static void start_mkcol(struct transfer_request *request)
{
	char *hex = sha1_to_hex(request->obj->sha1);
	struct active_request_slot *slot;

	request->url = get_remote_object_url(repo->url, hex, 1);

	slot = get_active_slot();
	slot->callback_func = process_response;
	slot->callback_data = request;
	curl_easy_setopt(slot->curl, CURLOPT_HTTPGET, 1); /* undo PUT setup */
	curl_easy_setopt(slot->curl, CURLOPT_URL, request->url);
	curl_easy_setopt(slot->curl, CURLOPT_ERRORBUFFER, request->errorstr);
	curl_easy_setopt(slot->curl, CURLOPT_CUSTOMREQUEST, DAV_MKCOL);
	curl_easy_setopt(slot->curl, CURLOPT_WRITEFUNCTION, fwrite_null);

	if (start_active_slot(slot)) {
		request->slot = slot;
		request->state = RUN_MKCOL;
	} else {
		request->state = ABORTED;
		free(request->url);
		request->url = NULL;
	}
}
#endif

static void start_fetch_packed(struct transfer_request *request)
{
	char *url;
	struct packed_git *target;
	FILE *packfile;
	char *filename;
	long prev_posn = 0;
	char range[RANGE_HEADER_SIZE];
	struct curl_slist *range_header = NULL;

	struct transfer_request *check_request = request_queue_head;
	struct active_request_slot *slot;

	target = find_sha1_pack(request->obj->sha1, repo->packs);
	if (!target) {
		fprintf(stderr, "Unable to fetch %s, will not be able to update server info refs\n", sha1_to_hex(request->obj->sha1));
		repo->can_update_info_refs = 0;
		release_request(request);
		return;
	}

	fprintf(stderr,	"Fetching pack %s\n", sha1_to_hex(target->sha1));
	fprintf(stderr, " which contains %s\n", sha1_to_hex(request->obj->sha1));

	filename = sha1_pack_name(target->sha1);
	snprintf(request->filename, sizeof(request->filename), "%s", filename);
	snprintf(request->tmpfile, sizeof(request->tmpfile),
		 "%s.temp", filename);

	url = xmalloc(strlen(repo->url) + 64);
	sprintf(url, "%sobjects/pack/pack-%s.pack",
		repo->url, sha1_to_hex(target->sha1));

	/* Make sure there isn't another open request for this pack */
	while (check_request) {
		if (check_request->state == RUN_FETCH_PACKED &&
		    !strcmp(check_request->url, url)) {
			free(url);
			release_request(request);
			return;
		}
		check_request = check_request->next;
	}

	packfile = fopen(request->tmpfile, "a");
	if (!packfile) {
		fprintf(stderr, "Unable to open local file %s for pack",
			request->tmpfile);
		repo->can_update_info_refs = 0;
		free(url);
		return;
	}

	slot = get_active_slot();
	slot->callback_func = process_response;
	slot->callback_data = request;
	request->slot = slot;
	request->local_stream = packfile;
	request->userData = target;

	request->url = url;
	curl_easy_setopt(slot->curl, CURLOPT_FILE, packfile);
	curl_easy_setopt(slot->curl, CURLOPT_WRITEFUNCTION, fwrite);
	curl_easy_setopt(slot->curl, CURLOPT_URL, url);
	curl_easy_setopt(slot->curl, CURLOPT_HTTPHEADER, no_pragma_header);
	slot->local = packfile;

	/* If there is data present from a previous transfer attempt,
	   resume where it left off */
	prev_posn = ftell(packfile);
	if (prev_posn>0) {
		if (push_verbosely)
			fprintf(stderr,
				"Resuming fetch of pack %s at byte %ld\n",
				sha1_to_hex(target->sha1), prev_posn);
		sprintf(range, "Range: bytes=%ld-", prev_posn);
		range_header = curl_slist_append(range_header, range);
		curl_easy_setopt(slot->curl, CURLOPT_HTTPHEADER, range_header);
	}

	/* Try to get the request started, abort the request on error */
	request->state = RUN_FETCH_PACKED;
	if (!start_active_slot(slot)) {
		fprintf(stderr, "Unable to start GET request\n");
		repo->can_update_info_refs = 0;
		release_request(request);
	}
}

static void start_put(struct transfer_request *request)
{
	char *hex = sha1_to_hex(request->obj->sha1);
	struct active_request_slot *slot;
	struct strbuf buf = STRBUF_INIT;
	enum object_type type;
	char hdr[50];
	void *unpacked;
	unsigned long len;
	int hdrlen;
	ssize_t size;
	z_stream stream;

	unpacked = read_sha1_file(request->obj->sha1, &type, &len);
	hdrlen = sprintf(hdr, "%s %lu", typename(type), len) + 1;

	/* Set it up */
	memset(&stream, 0, sizeof(stream));
	deflateInit(&stream, zlib_compression_level);
	size = deflateBound(&stream, len + hdrlen);
	strbuf_init(&request->buffer.buf, size);
	request->buffer.posn = 0;

	/* Compress it */
	stream.next_out = (unsigned char *)request->buffer.buf.buf;
	stream.avail_out = size;

	/* First header.. */
	stream.next_in = (void *)hdr;
	stream.avail_in = hdrlen;
	while (deflate(&stream, 0) == Z_OK)
		/* nothing */;

	/* Then the data itself.. */
	stream.next_in = unpacked;
	stream.avail_in = len;
	while (deflate(&stream, Z_FINISH) == Z_OK)
		/* nothing */;
	deflateEnd(&stream);
	free(unpacked);

	request->buffer.buf.len = stream.total_out;

	strbuf_addstr(&buf, "Destination: ");
	append_remote_object_url(&buf, repo->url, hex, 0);
	request->dest = strbuf_detach(&buf, NULL);

	append_remote_object_url(&buf, repo->url, hex, 0);
	strbuf_add(&buf, request->lock->tmpfile_suffix, 41);
	request->url = strbuf_detach(&buf, NULL);

	slot = get_active_slot();
	slot->callback_func = process_response;
	slot->callback_data = request;
	curl_easy_setopt(slot->curl, CURLOPT_INFILE, &request->buffer);
	curl_easy_setopt(slot->curl, CURLOPT_INFILESIZE, request->buffer.buf.len);
	curl_easy_setopt(slot->curl, CURLOPT_READFUNCTION, fread_buffer);
#ifndef NO_CURL_IOCTL
	curl_easy_setopt(slot->curl, CURLOPT_IOCTLFUNCTION, ioctl_buffer);
	curl_easy_setopt(slot->curl, CURLOPT_IOCTLDATA, &request->buffer);
#endif
	curl_easy_setopt(slot->curl, CURLOPT_WRITEFUNCTION, fwrite_null);
	curl_easy_setopt(slot->curl, CURLOPT_CUSTOMREQUEST, DAV_PUT);
	curl_easy_setopt(slot->curl, CURLOPT_UPLOAD, 1);
	curl_easy_setopt(slot->curl, CURLOPT_PUT, 1);
	curl_easy_setopt(slot->curl, CURLOPT_NOBODY, 0);
	curl_easy_setopt(slot->curl, CURLOPT_URL, request->url);

	if (start_active_slot(slot)) {
		request->slot = slot;
		request->state = RUN_PUT;
	} else {
		request->state = ABORTED;
		free(request->url);
		request->url = NULL;
	}
}

static void start_move(struct transfer_request *request)
{
	struct active_request_slot *slot;
	struct curl_slist *dav_headers = NULL;

	slot = get_active_slot();
	slot->callback_func = process_response;
	slot->callback_data = request;
	curl_easy_setopt(slot->curl, CURLOPT_HTTPGET, 1); /* undo PUT setup */
	curl_easy_setopt(slot->curl, CURLOPT_CUSTOMREQUEST, DAV_MOVE);
	dav_headers = curl_slist_append(dav_headers, request->dest);
	dav_headers = curl_slist_append(dav_headers, "Overwrite: T");
	curl_easy_setopt(slot->curl, CURLOPT_HTTPHEADER, dav_headers);
	curl_easy_setopt(slot->curl, CURLOPT_WRITEFUNCTION, fwrite_null);
	curl_easy_setopt(slot->curl, CURLOPT_URL, request->url);

	if (start_active_slot(slot)) {
		request->slot = slot;
		request->state = RUN_MOVE;
	} else {
		request->state = ABORTED;
		free(request->url);
		request->url = NULL;
	}
}

static int refresh_lock(struct remote_lock *lock)
{
	struct active_request_slot *slot;
	struct slot_results results;
	struct curl_slist *dav_headers;
	int rc = 0;

	lock->refreshing = 1;

	dav_headers = get_dav_token_headers(lock, DAV_HEADER_IF | DAV_HEADER_TIMEOUT);

	slot = get_active_slot();
	slot->results = &results;
	curl_easy_setopt(slot->curl, CURLOPT_HTTPGET, 1);
	curl_easy_setopt(slot->curl, CURLOPT_WRITEFUNCTION, fwrite_null);
	curl_easy_setopt(slot->curl, CURLOPT_URL, lock->url);
	curl_easy_setopt(slot->curl, CURLOPT_CUSTOMREQUEST, DAV_LOCK);
	curl_easy_setopt(slot->curl, CURLOPT_HTTPHEADER, dav_headers);

	if (start_active_slot(slot)) {
		run_active_slot(slot);
		if (results.curl_result != CURLE_OK) {
			fprintf(stderr, "LOCK HTTP error %ld\n",
				results.http_code);
		} else {
			lock->start_time = time(NULL);
			rc = 1;
		}
	}

	lock->refreshing = 0;
	curl_slist_free_all(dav_headers);

	return rc;
}

static void check_locks(void)
{
	struct remote_lock *lock = repo->locks;
	time_t current_time = time(NULL);
	int time_remaining;

	while (lock) {
		time_remaining = lock->start_time + lock->timeout -
			current_time;
		if (!lock->refreshing && time_remaining < LOCK_REFRESH) {
			if (!refresh_lock(lock)) {
				fprintf(stderr,
					"Unable to refresh lock for %s\n",
					lock->url);
				aborted = 1;
				return;
			}
		}
		lock = lock->next;
	}
}

static void release_request(struct transfer_request *request)
{
	struct transfer_request *entry = request_queue_head;

	if (request == request_queue_head) {
		request_queue_head = request->next;
	} else {
		while (entry->next != NULL && entry->next != request)
			entry = entry->next;
		if (entry->next == request)
			entry->next = entry->next->next;
	}

	if (request->local_fileno != -1)
		close(request->local_fileno);
	if (request->local_stream)
		fclose(request->local_stream);
	free(request->url);
	free(request);
}

static void finish_request(struct transfer_request *request)
{
	struct stat st;
	struct packed_git *target;
	struct packed_git **lst;
	struct active_request_slot *slot;

	request->curl_result = request->slot->curl_result;
	request->http_code = request->slot->http_code;
	slot = request->slot;
	request->slot = NULL;

	/* Keep locks active */
	check_locks();

	if (request->headers != NULL)
		curl_slist_free_all(request->headers);

	/* URL is reused for MOVE after PUT */
	if (request->state != RUN_PUT) {
		free(request->url);
		request->url = NULL;
	}

	if (request->state == RUN_MKCOL) {
		if (request->curl_result == CURLE_OK ||
		    request->http_code == 405) {
			remote_dir_exists[request->obj->sha1[0]] = 1;
			start_put(request);
		} else {
			fprintf(stderr, "MKCOL %s failed, aborting (%d/%ld)\n",
				sha1_to_hex(request->obj->sha1),
				request->curl_result, request->http_code);
			request->state = ABORTED;
			aborted = 1;
		}
	} else if (request->state == RUN_PUT) {
		if (request->curl_result == CURLE_OK) {
			start_move(request);
		} else {
			fprintf(stderr,	"PUT %s failed, aborting (%d/%ld)\n",
				sha1_to_hex(request->obj->sha1),
				request->curl_result, request->http_code);
			request->state = ABORTED;
			aborted = 1;
		}
	} else if (request->state == RUN_MOVE) {
		if (request->curl_result == CURLE_OK) {
			if (push_verbosely)
				fprintf(stderr, "    sent %s\n",
					sha1_to_hex(request->obj->sha1));
			request->obj->flags |= REMOTE;
			release_request(request);
		} else {
			fprintf(stderr, "MOVE %s failed, aborting (%d/%ld)\n",
				sha1_to_hex(request->obj->sha1),
				request->curl_result, request->http_code);
			request->state = ABORTED;
			aborted = 1;
		}
	} else if (request->state == RUN_FETCH_LOOSE) {
		close(request->local_fileno); request->local_fileno = -1;

		if (request->curl_result != CURLE_OK &&
		    request->http_code != 416) {
			if (stat(request->tmpfile, &st) == 0) {
				if (st.st_size == 0)
					unlink_or_warn(request->tmpfile);
			}
		} else {
			if (request->http_code == 416)
				warning("requested range invalid; we may already have all the data.");

			git_inflate_end(&request->stream);
			git_SHA1_Final(request->real_sha1, &request->c);
			if (request->zret != Z_STREAM_END) {
				unlink_or_warn(request->tmpfile);
			} else if (hashcmp(request->obj->sha1, request->real_sha1)) {
				unlink_or_warn(request->tmpfile);
			} else {
				request->rename =
					move_temp_to_file(
						request->tmpfile,
						request->filename);
				if (request->rename == 0) {
					request->obj->flags |= (LOCAL | REMOTE);
				}
			}
		}

		/* Try fetching packed if necessary */
		if (request->obj->flags & LOCAL)
			release_request(request);
		else
			start_fetch_packed(request);

	} else if (request->state == RUN_FETCH_PACKED) {
		if (request->curl_result != CURLE_OK) {
			fprintf(stderr, "Unable to get pack file %s\n%s",
				request->url, curl_errorstr);
			repo->can_update_info_refs = 0;
		} else {
			off_t pack_size = ftell(request->local_stream);

			fclose(request->local_stream);
			request->local_stream = NULL;
			slot->local = NULL;
			if (!move_temp_to_file(request->tmpfile,
					       request->filename)) {
				target = (struct packed_git *)request->userData;
				target->pack_size = pack_size;
				lst = &repo->packs;
				while (*lst != target)
					lst = &((*lst)->next);
				*lst = (*lst)->next;

				if (!verify_pack(target))
					install_packed_git(target);
				else
					repo->can_update_info_refs = 0;
			}
		}
		release_request(request);
	}
}

#ifdef USE_CURL_MULTI
static int fill_active_slot(void *unused)
{
	struct transfer_request *request;

	if (aborted)
		return 0;

	for (request = request_queue_head; request; request = request->next) {
		if (request->state == NEED_FETCH) {
			start_fetch_loose(request);
			return 1;
		} else if (pushing && request->state == NEED_PUSH) {
			if (remote_dir_exists[request->obj->sha1[0]] == 1) {
				start_put(request);
			} else {
				start_mkcol(request);
			}
			return 1;
		}
	}
	return 0;
}
#endif

static void get_remote_object_list(unsigned char parent);

static void add_fetch_request(struct object *obj)
{
	struct transfer_request *request;

	check_locks();

	/*
	 * Don't fetch the object if it's known to exist locally
	 * or is already in the request queue
	 */
	if (remote_dir_exists[obj->sha1[0]] == -1)
		get_remote_object_list(obj->sha1[0]);
	if (obj->flags & (LOCAL | FETCHING))
		return;

	obj->flags |= FETCHING;
	request = xmalloc(sizeof(*request));
	request->obj = obj;
	request->url = NULL;
	request->lock = NULL;
	request->headers = NULL;
	request->local_fileno = -1;
	request->local_stream = NULL;
	request->state = NEED_FETCH;
	request->next = request_queue_head;
	request_queue_head = request;

#ifdef USE_CURL_MULTI
	fill_active_slots();
	step_active_slots();
#endif
}

static int add_send_request(struct object *obj, struct remote_lock *lock)
{
	struct transfer_request *request = request_queue_head;
	struct packed_git *target;

	/* Keep locks active */
	check_locks();

	/*
	 * Don't push the object if it's known to exist on the remote
	 * or is already in the request queue
	 */
	if (remote_dir_exists[obj->sha1[0]] == -1)
		get_remote_object_list(obj->sha1[0]);
	if (obj->flags & (REMOTE | PUSHING))
		return 0;
	target = find_sha1_pack(obj->sha1, repo->packs);
	if (target) {
		obj->flags |= REMOTE;
		return 0;
	}

	obj->flags |= PUSHING;
	request = xmalloc(sizeof(*request));
	request->obj = obj;
	request->url = NULL;
	request->lock = lock;
	request->headers = NULL;
	request->local_fileno = -1;
	request->local_stream = NULL;
	request->state = NEED_PUSH;
	request->next = request_queue_head;
	request_queue_head = request;

#ifdef USE_CURL_MULTI
	fill_active_slots();
	step_active_slots();
#endif

	return 1;
}

static int fetch_index(unsigned char *sha1)
{
	char *hex = sha1_to_hex(sha1);
	char *filename;
	char *url;
	char tmpfile[PATH_MAX];
	long prev_posn = 0;
	char range[RANGE_HEADER_SIZE];
	struct curl_slist *range_header = NULL;

	FILE *indexfile;
	struct active_request_slot *slot;
	struct slot_results results;

	/* Don't use the index if the pack isn't there */
	url = xmalloc(strlen(repo->url) + 64);
	sprintf(url, "%sobjects/pack/pack-%s.pack", repo->url, hex);
	slot = get_active_slot();
	slot->results = &results;
	curl_easy_setopt(slot->curl, CURLOPT_URL, url);
	curl_easy_setopt(slot->curl, CURLOPT_NOBODY, 1);
	if (start_active_slot(slot)) {
		run_active_slot(slot);
		if (results.curl_result != CURLE_OK) {
			free(url);
			return error("Unable to verify pack %s is available",
				     hex);
		}
	} else {
		free(url);
		return error("Unable to start request");
	}

	if (has_pack_index(sha1)) {
		free(url);
		return 0;
	}

	if (push_verbosely)
		fprintf(stderr, "Getting index for pack %s\n", hex);

	sprintf(url, "%sobjects/pack/pack-%s.idx", repo->url, hex);

	filename = sha1_pack_index_name(sha1);
	snprintf(tmpfile, sizeof(tmpfile), "%s.temp", filename);
	indexfile = fopen(tmpfile, "a");
	if (!indexfile) {
		free(url);
		return error("Unable to open local file %s for pack index",
			     tmpfile);
	}

	slot = get_active_slot();
	slot->results = &results;
	curl_easy_setopt(slot->curl, CURLOPT_NOBODY, 0);
	curl_easy_setopt(slot->curl, CURLOPT_HTTPGET, 1);
	curl_easy_setopt(slot->curl, CURLOPT_FILE, indexfile);
	curl_easy_setopt(slot->curl, CURLOPT_WRITEFUNCTION, fwrite);
	curl_easy_setopt(slot->curl, CURLOPT_URL, url);
	curl_easy_setopt(slot->curl, CURLOPT_HTTPHEADER, no_pragma_header);
	slot->local = indexfile;

	/* If there is data present from a previous transfer attempt,
	   resume where it left off */
	prev_posn = ftell(indexfile);
	if (prev_posn>0) {
		if (push_verbosely)
			fprintf(stderr,
				"Resuming fetch of index for pack %s at byte %ld\n",
				hex, prev_posn);
		sprintf(range, "Range: bytes=%ld-", prev_posn);
		range_header = curl_slist_append(range_header, range);
		curl_easy_setopt(slot->curl, CURLOPT_HTTPHEADER, range_header);
	}

	if (start_active_slot(slot)) {
		run_active_slot(slot);
		if (results.curl_result != CURLE_OK) {
			free(url);
			fclose(indexfile);
			slot->local = NULL;
			return error("Unable to get pack index %s\n%s", url,
				     curl_errorstr);
		}
	} else {
		free(url);
		fclose(indexfile);
		slot->local = NULL;
		return error("Unable to start request");
	}

	free(url);
	fclose(indexfile);
	slot->local = NULL;

	return move_temp_to_file(tmpfile, filename);
}

static int setup_index(unsigned char *sha1)
{
	struct packed_git *new_pack;

	if (fetch_index(sha1))
		return -1;

	new_pack = parse_pack_index(sha1);
	new_pack->next = repo->packs;
	repo->packs = new_pack;
	return 0;
}

static int fetch_indices(void)
{
	unsigned char sha1[20];
	char *url;
	struct strbuf buffer = STRBUF_INIT;
	char *data;
	int i = 0;

	struct active_request_slot *slot;
	struct slot_results results;

	if (push_verbosely)
		fprintf(stderr, "Getting pack list\n");

	url = xmalloc(strlen(repo->url) + 20);
	sprintf(url, "%sobjects/info/packs", repo->url);

	slot = get_active_slot();
	slot->results = &results;
	curl_easy_setopt(slot->curl, CURLOPT_FILE, &buffer);
	curl_easy_setopt(slot->curl, CURLOPT_WRITEFUNCTION, fwrite_buffer);
	curl_easy_setopt(slot->curl, CURLOPT_URL, url);
	curl_easy_setopt(slot->curl, CURLOPT_HTTPHEADER, NULL);
	if (start_active_slot(slot)) {
		run_active_slot(slot);
		if (results.curl_result != CURLE_OK) {
			strbuf_release(&buffer);
			free(url);
			if (results.http_code == 404)
				return 0;
			else
				return error("%s", curl_errorstr);
		}
	} else {
		strbuf_release(&buffer);
		free(url);
		return error("Unable to start request");
	}
	free(url);

	data = buffer.buf;
	while (i < buffer.len) {
		switch (data[i]) {
		case 'P':
			i++;
			if (i + 52 < buffer.len &&
			    !prefixcmp(data + i, " pack-") &&
			    !prefixcmp(data + i + 46, ".pack\n")) {
				get_sha1_hex(data + i + 6, sha1);
				setup_index(sha1);
				i += 51;
				break;
			}
		default:
			while (data[i] != '\n')
				i++;
		}
		i++;
	}

	strbuf_release(&buffer);
	return 0;
}

static void one_remote_object(const char *hex)
{
	unsigned char sha1[20];
	struct object *obj;

	if (get_sha1_hex(hex, sha1) != 0)
		return;

	obj = lookup_object(sha1);
	if (!obj)
		obj = parse_object(sha1);

	/* Ignore remote objects that don't exist locally */
	if (!obj)
		return;

	obj->flags |= REMOTE;
	if (!object_list_contains(objects, obj))
		object_list_insert(obj, &objects);
}

static void handle_lockprop_ctx(struct xml_ctx *ctx, int tag_closed)
{
	int *lock_flags = (int *)ctx->userData;

	if (tag_closed) {
		if (!strcmp(ctx->name, DAV_CTX_LOCKENTRY)) {
			if ((*lock_flags & DAV_PROP_LOCKEX) &&
			    (*lock_flags & DAV_PROP_LOCKWR)) {
				*lock_flags |= DAV_LOCK_OK;
			}
			*lock_flags &= DAV_LOCK_OK;
		} else if (!strcmp(ctx->name, DAV_CTX_LOCKTYPE_WRITE)) {
			*lock_flags |= DAV_PROP_LOCKWR;
		} else if (!strcmp(ctx->name, DAV_CTX_LOCKTYPE_EXCLUSIVE)) {
			*lock_flags |= DAV_PROP_LOCKEX;
		}
	}
}

static void handle_new_lock_ctx(struct xml_ctx *ctx, int tag_closed)
{
	struct remote_lock *lock = (struct remote_lock *)ctx->userData;
	git_SHA_CTX sha_ctx;
	unsigned char lock_token_sha1[20];

	if (tag_closed && ctx->cdata) {
		if (!strcmp(ctx->name, DAV_ACTIVELOCK_OWNER)) {
			lock->owner = xmalloc(strlen(ctx->cdata) + 1);
			strcpy(lock->owner, ctx->cdata);
		} else if (!strcmp(ctx->name, DAV_ACTIVELOCK_TIMEOUT)) {
			if (!prefixcmp(ctx->cdata, "Second-"))
				lock->timeout =
					strtol(ctx->cdata + 7, NULL, 10);
		} else if (!strcmp(ctx->name, DAV_ACTIVELOCK_TOKEN)) {
			lock->token = xmalloc(strlen(ctx->cdata) + 1);
			strcpy(lock->token, ctx->cdata);

			git_SHA1_Init(&sha_ctx);
			git_SHA1_Update(&sha_ctx, lock->token, strlen(lock->token));
			git_SHA1_Final(lock_token_sha1, &sha_ctx);

			lock->tmpfile_suffix[0] = '_';
			memcpy(lock->tmpfile_suffix + 1, sha1_to_hex(lock_token_sha1), 40);
		}
	}
}

static void one_remote_ref(char *refname);

static void
xml_start_tag(void *userData, const char *name, const char **atts)
{
	struct xml_ctx *ctx = (struct xml_ctx *)userData;
	const char *c = strchr(name, ':');
	int new_len;

	if (c == NULL)
		c = name;
	else
		c++;

	new_len = strlen(ctx->name) + strlen(c) + 2;

	if (new_len > ctx->len) {
		ctx->name = xrealloc(ctx->name, new_len);
		ctx->len = new_len;
	}
	strcat(ctx->name, ".");
	strcat(ctx->name, c);

	free(ctx->cdata);
	ctx->cdata = NULL;

	ctx->userFunc(ctx, 0);
}

static void
xml_end_tag(void *userData, const char *name)
{
	struct xml_ctx *ctx = (struct xml_ctx *)userData;
	const char *c = strchr(name, ':');
	char *ep;

	ctx->userFunc(ctx, 1);

	if (c == NULL)
		c = name;
	else
		c++;

	ep = ctx->name + strlen(ctx->name) - strlen(c) - 1;
	*ep = 0;
}

static void
xml_cdata(void *userData, const XML_Char *s, int len)
{
	struct xml_ctx *ctx = (struct xml_ctx *)userData;
	free(ctx->cdata);
	ctx->cdata = xmemdupz(s, len);
}

static struct remote_lock *lock_remote(const char *path, long timeout)
{
	struct active_request_slot *slot;
	struct slot_results results;
	struct buffer out_buffer = { STRBUF_INIT, 0 };
	struct strbuf in_buffer = STRBUF_INIT;
	char *url;
	char *ep;
	char timeout_header[25];
	struct remote_lock *lock = NULL;
	struct curl_slist *dav_headers = NULL;
	struct xml_ctx ctx;
	char *escaped;

	url = xmalloc(strlen(repo->url) + strlen(path) + 1);
	sprintf(url, "%s%s", repo->url, path);

	/* Make sure leading directories exist for the remote ref */
	ep = strchr(url + strlen(repo->url) + 1, '/');
	while (ep) {
		char saved_character = ep[1];
		ep[1] = '\0';
		slot = get_active_slot();
		slot->results = &results;
		curl_easy_setopt(slot->curl, CURLOPT_HTTPGET, 1);
		curl_easy_setopt(slot->curl, CURLOPT_URL, url);
		curl_easy_setopt(slot->curl, CURLOPT_CUSTOMREQUEST, DAV_MKCOL);
		curl_easy_setopt(slot->curl, CURLOPT_WRITEFUNCTION, fwrite_null);
		if (start_active_slot(slot)) {
			run_active_slot(slot);
			if (results.curl_result != CURLE_OK &&
			    results.http_code != 405) {
				fprintf(stderr,
					"Unable to create branch path %s\n",
					url);
				free(url);
				return NULL;
			}
		} else {
			fprintf(stderr, "Unable to start MKCOL request\n");
			free(url);
			return NULL;
		}
		ep[1] = saved_character;
		ep = strchr(ep + 1, '/');
	}

	escaped = xml_entities(git_default_email);
	strbuf_addf(&out_buffer.buf, LOCK_REQUEST, escaped);
	free(escaped);

	sprintf(timeout_header, "Timeout: Second-%ld", timeout);
	dav_headers = curl_slist_append(dav_headers, timeout_header);
	dav_headers = curl_slist_append(dav_headers, "Content-Type: text/xml");

	slot = get_active_slot();
	slot->results = &results;
	curl_easy_setopt(slot->curl, CURLOPT_INFILE, &out_buffer);
	curl_easy_setopt(slot->curl, CURLOPT_INFILESIZE, out_buffer.buf.len);
	curl_easy_setopt(slot->curl, CURLOPT_READFUNCTION, fread_buffer);
#ifndef NO_CURL_IOCTL
	curl_easy_setopt(slot->curl, CURLOPT_IOCTLFUNCTION, ioctl_buffer);
	curl_easy_setopt(slot->curl, CURLOPT_IOCTLDATA, &out_buffer);
#endif
	curl_easy_setopt(slot->curl, CURLOPT_FILE, &in_buffer);
	curl_easy_setopt(slot->curl, CURLOPT_WRITEFUNCTION, fwrite_buffer);
	curl_easy_setopt(slot->curl, CURLOPT_URL, url);
	curl_easy_setopt(slot->curl, CURLOPT_UPLOAD, 1);
	curl_easy_setopt(slot->curl, CURLOPT_CUSTOMREQUEST, DAV_LOCK);
	curl_easy_setopt(slot->curl, CURLOPT_HTTPHEADER, dav_headers);

	lock = xcalloc(1, sizeof(*lock));
	lock->timeout = -1;

	if (start_active_slot(slot)) {
		run_active_slot(slot);
		if (results.curl_result == CURLE_OK) {
			XML_Parser parser = XML_ParserCreate(NULL);
			enum XML_Status result;
			ctx.name = xcalloc(10, 1);
			ctx.len = 0;
			ctx.cdata = NULL;
			ctx.userFunc = handle_new_lock_ctx;
			ctx.userData = lock;
			XML_SetUserData(parser, &ctx);
			XML_SetElementHandler(parser, xml_start_tag,
					      xml_end_tag);
			XML_SetCharacterDataHandler(parser, xml_cdata);
			result = XML_Parse(parser, in_buffer.buf,
					   in_buffer.len, 1);
			free(ctx.name);
			if (result != XML_STATUS_OK) {
				fprintf(stderr, "XML error: %s\n",
					XML_ErrorString(
						XML_GetErrorCode(parser)));
				lock->timeout = -1;
			}
			XML_ParserFree(parser);
		}
	} else {
		fprintf(stderr, "Unable to start LOCK request\n");
	}

	curl_slist_free_all(dav_headers);
	strbuf_release(&out_buffer.buf);
	strbuf_release(&in_buffer);

	if (lock->token == NULL || lock->timeout <= 0) {
		free(lock->token);
		free(lock->owner);
		free(url);
		free(lock);
		lock = NULL;
	} else {
		lock->url = url;
		lock->start_time = time(NULL);
		lock->next = repo->locks;
		repo->locks = lock;
	}

	return lock;
}

static int unlock_remote(struct remote_lock *lock)
{
	struct active_request_slot *slot;
	struct slot_results results;
	struct remote_lock *prev = repo->locks;
	struct curl_slist *dav_headers;
	int rc = 0;

	dav_headers = get_dav_token_headers(lock, DAV_HEADER_LOCK);

	slot = get_active_slot();
	slot->results = &results;
	curl_easy_setopt(slot->curl, CURLOPT_WRITEFUNCTION, fwrite_null);
	curl_easy_setopt(slot->curl, CURLOPT_URL, lock->url);
	curl_easy_setopt(slot->curl, CURLOPT_CUSTOMREQUEST, DAV_UNLOCK);
	curl_easy_setopt(slot->curl, CURLOPT_HTTPHEADER, dav_headers);

	if (start_active_slot(slot)) {
		run_active_slot(slot);
		if (results.curl_result == CURLE_OK)
			rc = 1;
		else
			fprintf(stderr, "UNLOCK HTTP error %ld\n",
				results.http_code);
	} else {
		fprintf(stderr, "Unable to start UNLOCK request\n");
	}

	curl_slist_free_all(dav_headers);

	if (repo->locks == lock) {
		repo->locks = lock->next;
	} else {
		while (prev && prev->next != lock)
			prev = prev->next;
		if (prev)
			prev->next = prev->next->next;
	}

	free(lock->owner);
	free(lock->url);
	free(lock->token);
	free(lock);

	return rc;
}

static void remove_locks(void)
{
	struct remote_lock *lock = repo->locks;

	fprintf(stderr, "Removing remote locks...\n");
	while (lock) {
		struct remote_lock *next = lock->next;
		unlock_remote(lock);
		lock = next;
	}
}

static void remove_locks_on_signal(int signo)
{
	remove_locks();
	sigchain_pop(signo);
	raise(signo);
}

static void remote_ls(const char *path, int flags,
		      void (*userFunc)(struct remote_ls_ctx *ls),
		      void *userData);

static void process_ls_object(struct remote_ls_ctx *ls)
{
	unsigned int *parent = (unsigned int *)ls->userData;
	char *path = ls->dentry_name;
	char *obj_hex;

	if (!strcmp(ls->path, ls->dentry_name) && (ls->flags & IS_DIR)) {
		remote_dir_exists[*parent] = 1;
		return;
	}

	if (strlen(path) != 49)
		return;
	path += 8;
	obj_hex = xmalloc(strlen(path));
	/* NB: path is not null-terminated, can not use strlcpy here */
	memcpy(obj_hex, path, 2);
	strcpy(obj_hex + 2, path + 3);
	one_remote_object(obj_hex);
	free(obj_hex);
}

static void process_ls_ref(struct remote_ls_ctx *ls)
{
	if (!strcmp(ls->path, ls->dentry_name) && (ls->dentry_flags & IS_DIR)) {
		fprintf(stderr, "  %s\n", ls->dentry_name);
		return;
	}

	if (!(ls->dentry_flags & IS_DIR))
		one_remote_ref(ls->dentry_name);
}

static void handle_remote_ls_ctx(struct xml_ctx *ctx, int tag_closed)
{
	struct remote_ls_ctx *ls = (struct remote_ls_ctx *)ctx->userData;

	if (tag_closed) {
		if (!strcmp(ctx->name, DAV_PROPFIND_RESP) && ls->dentry_name) {
			if (ls->dentry_flags & IS_DIR) {
				if (ls->flags & PROCESS_DIRS) {
					ls->userFunc(ls);
				}
				if (strcmp(ls->dentry_name, ls->path) &&
				    ls->flags & RECURSIVE) {
					remote_ls(ls->dentry_name,
						  ls->flags,
						  ls->userFunc,
						  ls->userData);
				}
			} else if (ls->flags & PROCESS_FILES) {
				ls->userFunc(ls);
			}
		} else if (!strcmp(ctx->name, DAV_PROPFIND_NAME) && ctx->cdata) {
			char *path = ctx->cdata;
			if (*ctx->cdata == 'h') {
				path = strstr(path, "//");
				if (path) {
					path = strchr(path+2, '/');
				}
			}
			if (path) {
				path += repo->path_len;
				ls->dentry_name = xstrdup(path);
			}
		} else if (!strcmp(ctx->name, DAV_PROPFIND_COLLECTION)) {
			ls->dentry_flags |= IS_DIR;
		}
	} else if (!strcmp(ctx->name, DAV_PROPFIND_RESP)) {
		free(ls->dentry_name);
		ls->dentry_name = NULL;
		ls->dentry_flags = 0;
	}
}

/*
 * NEEDSWORK: remote_ls() ignores info/refs on the remote side.  But it
 * should _only_ heed the information from that file, instead of trying to
 * determine the refs from the remote file system (badly: it does not even
 * know about packed-refs).
 */
static void remote_ls(const char *path, int flags,
		      void (*userFunc)(struct remote_ls_ctx *ls),
		      void *userData)
{
	char *url = xmalloc(strlen(repo->url) + strlen(path) + 1);
	struct active_request_slot *slot;
	struct slot_results results;
	struct strbuf in_buffer = STRBUF_INIT;
	struct buffer out_buffer = { STRBUF_INIT, 0 };
	struct curl_slist *dav_headers = NULL;
	struct xml_ctx ctx;
	struct remote_ls_ctx ls;

	ls.flags = flags;
	ls.path = xstrdup(path);
	ls.dentry_name = NULL;
	ls.dentry_flags = 0;
	ls.userData = userData;
	ls.userFunc = userFunc;

	sprintf(url, "%s%s", repo->url, path);

	strbuf_addf(&out_buffer.buf, PROPFIND_ALL_REQUEST);

	dav_headers = curl_slist_append(dav_headers, "Depth: 1");
	dav_headers = curl_slist_append(dav_headers, "Content-Type: text/xml");

	slot = get_active_slot();
	slot->results = &results;
	curl_easy_setopt(slot->curl, CURLOPT_INFILE, &out_buffer);
	curl_easy_setopt(slot->curl, CURLOPT_INFILESIZE, out_buffer.buf.len);
	curl_easy_setopt(slot->curl, CURLOPT_READFUNCTION, fread_buffer);
#ifndef NO_CURL_IOCTL
	curl_easy_setopt(slot->curl, CURLOPT_IOCTLFUNCTION, ioctl_buffer);
	curl_easy_setopt(slot->curl, CURLOPT_IOCTLDATA, &out_buffer);
#endif
	curl_easy_setopt(slot->curl, CURLOPT_FILE, &in_buffer);
	curl_easy_setopt(slot->curl, CURLOPT_WRITEFUNCTION, fwrite_buffer);
	curl_easy_setopt(slot->curl, CURLOPT_URL, url);
	curl_easy_setopt(slot->curl, CURLOPT_UPLOAD, 1);
	curl_easy_setopt(slot->curl, CURLOPT_CUSTOMREQUEST, DAV_PROPFIND);
	curl_easy_setopt(slot->curl, CURLOPT_HTTPHEADER, dav_headers);

	if (start_active_slot(slot)) {
		run_active_slot(slot);
		if (results.curl_result == CURLE_OK) {
			XML_Parser parser = XML_ParserCreate(NULL);
			enum XML_Status result;
			ctx.name = xcalloc(10, 1);
			ctx.len = 0;
			ctx.cdata = NULL;
			ctx.userFunc = handle_remote_ls_ctx;
			ctx.userData = &ls;
			XML_SetUserData(parser, &ctx);
			XML_SetElementHandler(parser, xml_start_tag,
					      xml_end_tag);
			XML_SetCharacterDataHandler(parser, xml_cdata);
			result = XML_Parse(parser, in_buffer.buf,
					   in_buffer.len, 1);
			free(ctx.name);

			if (result != XML_STATUS_OK) {
				fprintf(stderr, "XML error: %s\n",
					XML_ErrorString(
						XML_GetErrorCode(parser)));
			}
			XML_ParserFree(parser);
		}
	} else {
		fprintf(stderr, "Unable to start PROPFIND request\n");
	}

	free(ls.path);
	free(url);
	strbuf_release(&out_buffer.buf);
	strbuf_release(&in_buffer);
	curl_slist_free_all(dav_headers);
}

static void get_remote_object_list(unsigned char parent)
{
	char path[] = "objects/XX/";
	static const char hex[] = "0123456789abcdef";
	unsigned int val = parent;

	path[8] = hex[val >> 4];
	path[9] = hex[val & 0xf];
	remote_dir_exists[val] = 0;
	remote_ls(path, (PROCESS_FILES | PROCESS_DIRS),
		  process_ls_object, &val);
}

static int locking_available(void)
{
	struct active_request_slot *slot;
	struct slot_results results;
	struct strbuf in_buffer = STRBUF_INIT;
	struct buffer out_buffer = { STRBUF_INIT, 0 };
	struct curl_slist *dav_headers = NULL;
	struct xml_ctx ctx;
	int lock_flags = 0;
	char *escaped;

	escaped = xml_entities(repo->url);
	strbuf_addf(&out_buffer.buf, PROPFIND_SUPPORTEDLOCK_REQUEST, escaped);
	free(escaped);

	dav_headers = curl_slist_append(dav_headers, "Depth: 0");
	dav_headers = curl_slist_append(dav_headers, "Content-Type: text/xml");

	slot = get_active_slot();
	slot->results = &results;
	curl_easy_setopt(slot->curl, CURLOPT_INFILE, &out_buffer);
	curl_easy_setopt(slot->curl, CURLOPT_INFILESIZE, out_buffer.buf.len);
	curl_easy_setopt(slot->curl, CURLOPT_READFUNCTION, fread_buffer);
#ifndef NO_CURL_IOCTL
	curl_easy_setopt(slot->curl, CURLOPT_IOCTLFUNCTION, ioctl_buffer);
	curl_easy_setopt(slot->curl, CURLOPT_IOCTLDATA, &out_buffer);
#endif
	curl_easy_setopt(slot->curl, CURLOPT_FILE, &in_buffer);
	curl_easy_setopt(slot->curl, CURLOPT_WRITEFUNCTION, fwrite_buffer);
	curl_easy_setopt(slot->curl, CURLOPT_URL, repo->url);
	curl_easy_setopt(slot->curl, CURLOPT_UPLOAD, 1);
	curl_easy_setopt(slot->curl, CURLOPT_CUSTOMREQUEST, DAV_PROPFIND);
	curl_easy_setopt(slot->curl, CURLOPT_HTTPHEADER, dav_headers);

	if (start_active_slot(slot)) {
		run_active_slot(slot);
		if (results.curl_result == CURLE_OK) {
			XML_Parser parser = XML_ParserCreate(NULL);
			enum XML_Status result;
			ctx.name = xcalloc(10, 1);
			ctx.len = 0;
			ctx.cdata = NULL;
			ctx.userFunc = handle_lockprop_ctx;
			ctx.userData = &lock_flags;
			XML_SetUserData(parser, &ctx);
			XML_SetElementHandler(parser, xml_start_tag,
					      xml_end_tag);
			result = XML_Parse(parser, in_buffer.buf,
					   in_buffer.len, 1);
			free(ctx.name);

			if (result != XML_STATUS_OK) {
				fprintf(stderr, "XML error: %s\n",
					XML_ErrorString(
						XML_GetErrorCode(parser)));
				lock_flags = 0;
			}
			XML_ParserFree(parser);
			if (!lock_flags)
				error("no DAV locking support on %s",
				      repo->url);

		} else {
			error("Cannot access URL %s, return code %d",
			      repo->url, results.curl_result);
			lock_flags = 0;
		}
	} else {
		error("Unable to start PROPFIND request on %s", repo->url);
	}

	strbuf_release(&out_buffer.buf);
	strbuf_release(&in_buffer);
	curl_slist_free_all(dav_headers);

	return lock_flags;
}

static struct object_list **add_one_object(struct object *obj, struct object_list **p)
{
	struct object_list *entry = xmalloc(sizeof(struct object_list));
	entry->item = obj;
	entry->next = *p;
	*p = entry;
	return &entry->next;
}

static struct object_list **process_blob(struct blob *blob,
					 struct object_list **p,
					 struct name_path *path,
					 const char *name)
{
	struct object *obj = &blob->object;

	obj->flags |= LOCAL;

	if (obj->flags & (UNINTERESTING | SEEN))
		return p;

	obj->flags |= SEEN;
	return add_one_object(obj, p);
}

static struct object_list **process_tree(struct tree *tree,
					 struct object_list **p,
					 struct name_path *path,
					 const char *name)
{
	struct object *obj = &tree->object;
	struct tree_desc desc;
	struct name_entry entry;
	struct name_path me;

	obj->flags |= LOCAL;

	if (obj->flags & (UNINTERESTING | SEEN))
		return p;
	if (parse_tree(tree) < 0)
		die("bad tree object %s", sha1_to_hex(obj->sha1));

	obj->flags |= SEEN;
	name = xstrdup(name);
	p = add_one_object(obj, p);
	me.up = path;
	me.elem = name;
	me.elem_len = strlen(name);

	init_tree_desc(&desc, tree->buffer, tree->size);

	while (tree_entry(&desc, &entry))
		switch (object_type(entry.mode)) {
		case OBJ_TREE:
			p = process_tree(lookup_tree(entry.sha1), p, &me, name);
			break;
		case OBJ_BLOB:
			p = process_blob(lookup_blob(entry.sha1), p, &me, name);
			break;
		default:
			/* Subproject commit - not in this repository */
			break;
		}

	free(tree->buffer);
	tree->buffer = NULL;
	return p;
}

static int get_delta(struct rev_info *revs, struct remote_lock *lock)
{
	int i;
	struct commit *commit;
	struct object_list **p = &objects;
	int count = 0;

	while ((commit = get_revision(revs)) != NULL) {
		p = process_tree(commit->tree, p, NULL, "");
		commit->object.flags |= LOCAL;
		if (!(commit->object.flags & UNINTERESTING))
			count += add_send_request(&commit->object, lock);
	}

	for (i = 0; i < revs->pending.nr; i++) {
		struct object_array_entry *entry = revs->pending.objects + i;
		struct object *obj = entry->item;
		const char *name = entry->name;

		if (obj->flags & (UNINTERESTING | SEEN))
			continue;
		if (obj->type == OBJ_TAG) {
			obj->flags |= SEEN;
			p = add_one_object(obj, p);
			continue;
		}
		if (obj->type == OBJ_TREE) {
			p = process_tree((struct tree *)obj, p, NULL, name);
			continue;
		}
		if (obj->type == OBJ_BLOB) {
			p = process_blob((struct blob *)obj, p, NULL, name);
			continue;
		}
		die("unknown pending object %s (%s)", sha1_to_hex(obj->sha1), name);
	}

	while (objects) {
		if (!(objects->item->flags & UNINTERESTING))
			count += add_send_request(objects->item, lock);
		objects = objects->next;
	}

	return count;
}

static int update_remote(unsigned char *sha1, struct remote_lock *lock)
{
	struct active_request_slot *slot;
	struct slot_results results;
	struct buffer out_buffer = { STRBUF_INIT, 0 };
	struct curl_slist *dav_headers;

	dav_headers = get_dav_token_headers(lock, DAV_HEADER_IF);

	strbuf_addf(&out_buffer.buf, "%s\n", sha1_to_hex(sha1));

	slot = get_active_slot();
	slot->results = &results;
	curl_easy_setopt(slot->curl, CURLOPT_INFILE, &out_buffer);
	curl_easy_setopt(slot->curl, CURLOPT_INFILESIZE, out_buffer.buf.len);
	curl_easy_setopt(slot->curl, CURLOPT_READFUNCTION, fread_buffer);
#ifndef NO_CURL_IOCTL
	curl_easy_setopt(slot->curl, CURLOPT_IOCTLFUNCTION, ioctl_buffer);
	curl_easy_setopt(slot->curl, CURLOPT_IOCTLDATA, &out_buffer);
#endif
	curl_easy_setopt(slot->curl, CURLOPT_WRITEFUNCTION, fwrite_null);
	curl_easy_setopt(slot->curl, CURLOPT_CUSTOMREQUEST, DAV_PUT);
	curl_easy_setopt(slot->curl, CURLOPT_HTTPHEADER, dav_headers);
	curl_easy_setopt(slot->curl, CURLOPT_UPLOAD, 1);
	curl_easy_setopt(slot->curl, CURLOPT_PUT, 1);
	curl_easy_setopt(slot->curl, CURLOPT_URL, lock->url);

	if (start_active_slot(slot)) {
		run_active_slot(slot);
		strbuf_release(&out_buffer.buf);
		if (results.curl_result != CURLE_OK) {
			fprintf(stderr,
				"PUT error: curl result=%d, HTTP code=%ld\n",
				results.curl_result, results.http_code);
			/* We should attempt recovery? */
			return 0;
		}
	} else {
		strbuf_release(&out_buffer.buf);
		fprintf(stderr, "Unable to start PUT request\n");
		return 0;
	}

	return 1;
}

static struct ref *remote_refs, **remote_tail;

static void one_remote_ref(char *refname)
{
	struct ref *ref;
	struct object *obj;

	ref = alloc_ref(refname);

	if (http_fetch_ref(repo->url, ref) != 0) {
		fprintf(stderr,
			"Unable to fetch ref %s from %s\n",
			refname, repo->url);
		free(ref);
		return;
	}

	/*
	 * Fetch a copy of the object if it doesn't exist locally - it
	 * may be required for updating server info later.
	 */
	if (repo->can_update_info_refs && !has_sha1_file(ref->old_sha1)) {
		obj = lookup_unknown_object(ref->old_sha1);
		if (obj) {
			fprintf(stderr,	"  fetch %s for %s\n",
				sha1_to_hex(ref->old_sha1), refname);
			add_fetch_request(obj);
		}
	}

	*remote_tail = ref;
	remote_tail = &ref->next;
}

static void get_dav_remote_heads(void)
{
	remote_tail = &remote_refs;
	remote_ls("refs/", (PROCESS_FILES | PROCESS_DIRS | RECURSIVE), process_ls_ref, NULL);
}

static int is_zero_sha1(const unsigned char *sha1)
{
	int i;

	for (i = 0; i < 20; i++) {
		if (*sha1++)
			return 0;
	}
	return 1;
}

static void add_remote_info_ref(struct remote_ls_ctx *ls)
{
	struct strbuf *buf = (struct strbuf *)ls->userData;
	struct object *o;
	int len;
	char *ref_info;
	struct ref *ref;

	ref = alloc_ref(ls->dentry_name);

	if (http_fetch_ref(repo->url, ref) != 0) {
		fprintf(stderr,
			"Unable to fetch ref %s from %s\n",
			ls->dentry_name, repo->url);
		aborted = 1;
		free(ref);
		return;
	}

	o = parse_object(ref->old_sha1);
	if (!o) {
		fprintf(stderr,
			"Unable to parse object %s for remote ref %s\n",
			sha1_to_hex(ref->old_sha1), ls->dentry_name);
		aborted = 1;
		free(ref);
		return;
	}

	len = strlen(ls->dentry_name) + 42;
	ref_info = xcalloc(len + 1, 1);
	sprintf(ref_info, "%s	%s\n",
		sha1_to_hex(ref->old_sha1), ls->dentry_name);
	fwrite_buffer(ref_info, 1, len, buf);
	free(ref_info);

	if (o->type == OBJ_TAG) {
		o = deref_tag(o, ls->dentry_name, 0);
		if (o) {
			len = strlen(ls->dentry_name) + 45;
			ref_info = xcalloc(len + 1, 1);
			sprintf(ref_info, "%s	%s^{}\n",
				sha1_to_hex(o->sha1), ls->dentry_name);
			fwrite_buffer(ref_info, 1, len, buf);
			free(ref_info);
		}
	}
	free(ref);
}

static void update_remote_info_refs(struct remote_lock *lock)
{
	struct buffer buffer = { STRBUF_INIT, 0 };
	struct active_request_slot *slot;
	struct slot_results results;
	struct curl_slist *dav_headers;

	remote_ls("refs/", (PROCESS_FILES | RECURSIVE),
		  add_remote_info_ref, &buffer.buf);
	if (!aborted) {
		dav_headers = get_dav_token_headers(lock, DAV_HEADER_IF);

		slot = get_active_slot();
		slot->results = &results;
		curl_easy_setopt(slot->curl, CURLOPT_INFILE, &buffer);
		curl_easy_setopt(slot->curl, CURLOPT_INFILESIZE, buffer.buf.len);
		curl_easy_setopt(slot->curl, CURLOPT_READFUNCTION, fread_buffer);
#ifndef NO_CURL_IOCTL
		curl_easy_setopt(slot->curl, CURLOPT_IOCTLFUNCTION, ioctl_buffer);
		curl_easy_setopt(slot->curl, CURLOPT_IOCTLDATA, &buffer);
#endif
		curl_easy_setopt(slot->curl, CURLOPT_WRITEFUNCTION, fwrite_null);
		curl_easy_setopt(slot->curl, CURLOPT_CUSTOMREQUEST, DAV_PUT);
		curl_easy_setopt(slot->curl, CURLOPT_HTTPHEADER, dav_headers);
		curl_easy_setopt(slot->curl, CURLOPT_UPLOAD, 1);
		curl_easy_setopt(slot->curl, CURLOPT_PUT, 1);
		curl_easy_setopt(slot->curl, CURLOPT_URL, lock->url);

		if (start_active_slot(slot)) {
			run_active_slot(slot);
			if (results.curl_result != CURLE_OK) {
				fprintf(stderr,
					"PUT error: curl result=%d, HTTP code=%ld\n",
					results.curl_result, results.http_code);
			}
		}
	}
	strbuf_release(&buffer.buf);
}

static int remote_exists(const char *path)
{
	char *url = xmalloc(strlen(repo->url) + strlen(path) + 1);
	struct active_request_slot *slot;
	struct slot_results results;
	int ret = -1;

	sprintf(url, "%s%s", repo->url, path);

	slot = get_active_slot();
	slot->results = &results;
	curl_easy_setopt(slot->curl, CURLOPT_URL, url);
	curl_easy_setopt(slot->curl, CURLOPT_NOBODY, 1);

	if (start_active_slot(slot)) {
		run_active_slot(slot);
		if (results.http_code == 404)
			ret = 0;
		else if (results.curl_result == CURLE_OK)
			ret = 1;
		else
			fprintf(stderr, "HEAD HTTP error %ld\n", results.http_code);
	} else {
		fprintf(stderr, "Unable to start HEAD request\n");
	}

	free(url);
	return ret;
}

static void fetch_symref(const char *path, char **symref, unsigned char *sha1)
{
	char *url;
	struct strbuf buffer = STRBUF_INIT;
	struct active_request_slot *slot;
	struct slot_results results;

	url = xmalloc(strlen(repo->url) + strlen(path) + 1);
	sprintf(url, "%s%s", repo->url, path);

	slot = get_active_slot();
	slot->results = &results;
	curl_easy_setopt(slot->curl, CURLOPT_FILE, &buffer);
	curl_easy_setopt(slot->curl, CURLOPT_WRITEFUNCTION, fwrite_buffer);
	curl_easy_setopt(slot->curl, CURLOPT_HTTPHEADER, NULL);
	curl_easy_setopt(slot->curl, CURLOPT_URL, url);
	if (start_active_slot(slot)) {
		run_active_slot(slot);
		if (results.curl_result != CURLE_OK) {
			die("Couldn't get %s for remote symref\n%s",
			    url, curl_errorstr);
		}
	} else {
		die("Unable to start remote symref request");
	}
	free(url);

	free(*symref);
	*symref = NULL;
	hashclr(sha1);

	if (buffer.len == 0)
		return;

	/* If it's a symref, set the refname; otherwise try for a sha1 */
	if (!prefixcmp((char *)buffer.buf, "ref: ")) {
		*symref = xmemdupz((char *)buffer.buf + 5, buffer.len - 6);
	} else {
		get_sha1_hex(buffer.buf, sha1);
	}

	strbuf_release(&buffer);
}

static int verify_merge_base(unsigned char *head_sha1, unsigned char *branch_sha1)
{
	struct commit *head = lookup_commit(head_sha1);
	struct commit *branch = lookup_commit(branch_sha1);
	struct commit_list *merge_bases = get_merge_bases(head, branch, 1);

	return (merge_bases && !merge_bases->next && merge_bases->item == branch);
}

static int delete_remote_branch(char *pattern, int force)
{
	struct ref *refs = remote_refs;
	struct ref *remote_ref = NULL;
	unsigned char head_sha1[20];
	char *symref = NULL;
	int match;
	int patlen = strlen(pattern);
	int i;
	struct active_request_slot *slot;
	struct slot_results results;
	char *url;

	/* Find the remote branch(es) matching the specified branch name */
	for (match = 0; refs; refs = refs->next) {
		char *name = refs->name;
		int namelen = strlen(name);
		if (namelen < patlen ||
		    memcmp(name + namelen - patlen, pattern, patlen))
			continue;
		if (namelen != patlen && name[namelen - patlen - 1] != '/')
			continue;
		match++;
		remote_ref = refs;
	}
	if (match == 0)
		return error("No remote branch matches %s", pattern);
	if (match != 1)
		return error("More than one remote branch matches %s",
			     pattern);

	/*
	 * Remote HEAD must be a symref (not exactly foolproof; a remote
	 * symlink to a symref will look like a symref)
	 */
	fetch_symref("HEAD", &symref, head_sha1);
	if (!symref)
		return error("Remote HEAD is not a symref");

	/* Remote branch must not be the remote HEAD */
	for (i=0; symref && i<MAXDEPTH; i++) {
		if (!strcmp(remote_ref->name, symref))
			return error("Remote branch %s is the current HEAD",
				     remote_ref->name);
		fetch_symref(symref, &symref, head_sha1);
	}

	/* Run extra sanity checks if delete is not forced */
	if (!force) {
		/* Remote HEAD must resolve to a known object */
		if (symref)
			return error("Remote HEAD symrefs too deep");
		if (is_zero_sha1(head_sha1))
			return error("Unable to resolve remote HEAD");
		if (!has_sha1_file(head_sha1))
			return error("Remote HEAD resolves to object %s\nwhich does not exist locally, perhaps you need to fetch?", sha1_to_hex(head_sha1));

		/* Remote branch must resolve to a known object */
		if (is_zero_sha1(remote_ref->old_sha1))
			return error("Unable to resolve remote branch %s",
				     remote_ref->name);
		if (!has_sha1_file(remote_ref->old_sha1))
			return error("Remote branch %s resolves to object %s\nwhich does not exist locally, perhaps you need to fetch?", remote_ref->name, sha1_to_hex(remote_ref->old_sha1));

		/* Remote branch must be an ancestor of remote HEAD */
		if (!verify_merge_base(head_sha1, remote_ref->old_sha1)) {
			return error("The branch '%s' is not an ancestor "
				     "of your current HEAD.\n"
				     "If you are sure you want to delete it,"
				     " run:\n\t'git http-push -D %s %s'",
				     remote_ref->name, repo->url, pattern);
		}
	}

	/* Send delete request */
	fprintf(stderr, "Removing remote branch '%s'\n", remote_ref->name);
	if (dry_run)
		return 0;
	url = xmalloc(strlen(repo->url) + strlen(remote_ref->name) + 1);
	sprintf(url, "%s%s", repo->url, remote_ref->name);
	slot = get_active_slot();
	slot->results = &results;
	curl_easy_setopt(slot->curl, CURLOPT_HTTPGET, 1);
	curl_easy_setopt(slot->curl, CURLOPT_WRITEFUNCTION, fwrite_null);
	curl_easy_setopt(slot->curl, CURLOPT_URL, url);
	curl_easy_setopt(slot->curl, CURLOPT_CUSTOMREQUEST, DAV_DELETE);
	if (start_active_slot(slot)) {
		run_active_slot(slot);
		free(url);
		if (results.curl_result != CURLE_OK)
			return error("DELETE request failed (%d/%ld)\n",
				     results.curl_result, results.http_code);
	} else {
		free(url);
		return error("Unable to start DELETE request");
	}

	return 0;
}

int main(int argc, char **argv)
{
	struct transfer_request *request;
	struct transfer_request *next_request;
	int nr_refspec = 0;
	char **refspec = NULL;
	struct remote_lock *ref_lock = NULL;
	struct remote_lock *info_ref_lock = NULL;
	struct rev_info revs;
	int delete_branch = 0;
	int force_delete = 0;
	int objects_to_send;
	int rc = 0;
	int i;
	int new_refs;
	struct ref *ref, *local_refs;
	struct remote *remote;
	char *rewritten_url = NULL;

	git_extract_argv0_path(argv[0]);

	setup_git_directory();

	repo = xcalloc(sizeof(*repo), 1);

	argv++;
	for (i = 1; i < argc; i++, argv++) {
		char *arg = *argv;

		if (*arg == '-') {
			if (!strcmp(arg, "--all")) {
				push_all = MATCH_REFS_ALL;
				continue;
			}
			if (!strcmp(arg, "--force")) {
				force_all = 1;
				continue;
			}
			if (!strcmp(arg, "--dry-run")) {
				dry_run = 1;
				continue;
			}
			if (!strcmp(arg, "--verbose")) {
				push_verbosely = 1;
				continue;
			}
			if (!strcmp(arg, "-d")) {
				delete_branch = 1;
				continue;
			}
			if (!strcmp(arg, "-D")) {
				delete_branch = 1;
				force_delete = 1;
				continue;
			}
		}
		if (!repo->url) {
			char *path = strstr(arg, "//");
			repo->url = arg;
			repo->path_len = strlen(arg);
			if (path) {
				repo->path = strchr(path+2, '/');
				if (repo->path)
					repo->path_len = strlen(repo->path);
			}
			continue;
		}
		refspec = argv;
		nr_refspec = argc - i;
		break;
	}

#ifndef USE_CURL_MULTI
	die("git-push is not available for http/https repository when not compiled with USE_CURL_MULTI");
#endif

	if (!repo->url)
		usage(http_push_usage);

	if (delete_branch && nr_refspec != 1)
		die("You must specify only one branch name when deleting a remote branch");

	memset(remote_dir_exists, -1, 256);

	/*
	 * Create a minimum remote by hand to give to http_init(),
	 * primarily to allow it to look at the URL.
	 */
	remote = xcalloc(sizeof(*remote), 1);
	ALLOC_GROW(remote->url, remote->url_nr + 1, remote->url_alloc);
	remote->url[remote->url_nr++] = repo->url;
	http_init(remote);

	no_pragma_header = curl_slist_append(no_pragma_header, "Pragma:");

	if (repo->url && repo->url[strlen(repo->url)-1] != '/') {
		rewritten_url = xmalloc(strlen(repo->url)+2);
		strcpy(rewritten_url, repo->url);
		strcat(rewritten_url, "/");
		repo->path = rewritten_url + (repo->path - repo->url);
		repo->path_len++;
		repo->url = rewritten_url;
	}

	/* Verify DAV compliance/lock support */
	if (!locking_available()) {
		rc = 1;
		goto cleanup;
	}

	sigchain_push_common(remove_locks_on_signal);

	/* Check whether the remote has server info files */
	repo->can_update_info_refs = 0;
	repo->has_info_refs = remote_exists("info/refs");
	repo->has_info_packs = remote_exists("objects/info/packs");
	if (repo->has_info_refs) {
		info_ref_lock = lock_remote("info/refs", LOCK_TIME);
		if (info_ref_lock)
			repo->can_update_info_refs = 1;
		else {
			error("cannot lock existing info/refs");
			rc = 1;
			goto cleanup;
		}
	}
	if (repo->has_info_packs)
		fetch_indices();

	/* Get a list of all local and remote heads to validate refspecs */
	local_refs = get_local_heads();
	fprintf(stderr, "Fetching remote heads...\n");
	get_dav_remote_heads();

	/* Remove a remote branch if -d or -D was specified */
	if (delete_branch) {
		if (delete_remote_branch(refspec[0], force_delete) == -1)
			fprintf(stderr, "Unable to delete remote branch %s\n",
				refspec[0]);
		goto cleanup;
	}

	/* match them up */
	if (!remote_tail)
		remote_tail = &remote_refs;
	if (match_refs(local_refs, remote_refs, &remote_tail,
		       nr_refspec, (const char **) refspec, push_all)) {
		rc = -1;
		goto cleanup;
	}
	if (!remote_refs) {
		fprintf(stderr, "No refs in common and none specified; doing nothing.\n");
		rc = 0;
		goto cleanup;
	}

	new_refs = 0;
	for (ref = remote_refs; ref; ref = ref->next) {
		char old_hex[60], *new_hex;
		const char *commit_argv[4];
		int commit_argc;
		char *new_sha1_hex, *old_sha1_hex;

		if (!ref->peer_ref)
			continue;

		if (is_zero_sha1(ref->peer_ref->new_sha1)) {
			if (delete_remote_branch(ref->name, 1) == -1) {
				error("Could not remove %s", ref->name);
				rc = -4;
			}
			new_refs++;
			continue;
		}

		if (!hashcmp(ref->old_sha1, ref->peer_ref->new_sha1)) {
			if (push_verbosely || 1)
				fprintf(stderr, "'%s': up-to-date\n", ref->name);
			continue;
		}

		if (!force_all &&
		    !is_zero_sha1(ref->old_sha1) &&
		    !ref->force) {
			if (!has_sha1_file(ref->old_sha1) ||
			    !ref_newer(ref->peer_ref->new_sha1,
				       ref->old_sha1)) {
				/*
				 * We do not have the remote ref, or
				 * we know that the remote ref is not
				 * an ancestor of what we are trying to
				 * push.  Either way this can be losing
				 * commits at the remote end and likely
				 * we were not up to date to begin with.
				 */
				error("remote '%s' is not an ancestor of\n"
				      "local '%s'.\n"
				      "Maybe you are not up-to-date and "
				      "need to pull first?",
				      ref->name,
				      ref->peer_ref->name);
				rc = -2;
				continue;
			}
		}
		hashcpy(ref->new_sha1, ref->peer_ref->new_sha1);
		new_refs++;
		strcpy(old_hex, sha1_to_hex(ref->old_sha1));
		new_hex = sha1_to_hex(ref->new_sha1);

		fprintf(stderr, "updating '%s'", ref->name);
		if (strcmp(ref->name, ref->peer_ref->name))
			fprintf(stderr, " using '%s'", ref->peer_ref->name);
		fprintf(stderr, "\n  from %s\n  to   %s\n", old_hex, new_hex);
		if (dry_run)
			continue;

		/* Lock remote branch ref */
		ref_lock = lock_remote(ref->name, LOCK_TIME);
		if (ref_lock == NULL) {
			fprintf(stderr, "Unable to lock remote branch %s\n",
				ref->name);
			rc = 1;
			continue;
		}

		/* Set up revision info for this refspec */
		commit_argc = 3;
		new_sha1_hex = xstrdup(sha1_to_hex(ref->new_sha1));
		old_sha1_hex = NULL;
		commit_argv[1] = "--objects";
		commit_argv[2] = new_sha1_hex;
		if (!push_all && !is_zero_sha1(ref->old_sha1)) {
			old_sha1_hex = xmalloc(42);
			sprintf(old_sha1_hex, "^%s",
				sha1_to_hex(ref->old_sha1));
			commit_argv[3] = old_sha1_hex;
			commit_argc++;
		}
		init_revisions(&revs, setup_git_directory());
		setup_revisions(commit_argc, commit_argv, &revs, NULL);
		revs.edge_hint = 0; /* just in case */
		free(new_sha1_hex);
		if (old_sha1_hex) {
			free(old_sha1_hex);
			commit_argv[1] = NULL;
		}

		/* Generate a list of objects that need to be pushed */
		pushing = 0;
		if (prepare_revision_walk(&revs))
			die("revision walk setup failed");
		mark_edges_uninteresting(revs.commits, &revs, NULL);
		objects_to_send = get_delta(&revs, ref_lock);
		finish_all_active_slots();

		/* Push missing objects to remote, this would be a
		   convenient time to pack them first if appropriate. */
		pushing = 1;
		if (objects_to_send)
			fprintf(stderr, "    sending %d objects\n",
				objects_to_send);
#ifdef USE_CURL_MULTI
		fill_active_slots();
		add_fill_function(NULL, fill_active_slot);
#endif
		do {
			finish_all_active_slots();
#ifdef USE_CURL_MULTI
			fill_active_slots();
#endif
		} while (request_queue_head && !aborted);

		/* Update the remote branch if all went well */
		if (aborted || !update_remote(ref->new_sha1, ref_lock))
			rc = 1;

		if (!rc)
			fprintf(stderr, "    done\n");
		unlock_remote(ref_lock);
		check_locks();
	}

	/* Update remote server info if appropriate */
	if (repo->has_info_refs && new_refs) {
		if (info_ref_lock && repo->can_update_info_refs) {
			fprintf(stderr, "Updating remote server info\n");
			if (!dry_run)
				update_remote_info_refs(info_ref_lock);
		} else {
			fprintf(stderr, "Unable to update server info\n");
		}
	}

 cleanup:
	free(rewritten_url);
	if (info_ref_lock)
		unlock_remote(info_ref_lock);
	free(repo);

	curl_slist_free_all(no_pragma_header);

	http_cleanup();

	request = request_queue_head;
	while (request != NULL) {
		next_request = request->next;
		release_request(request);
		request = next_request;
	}

	return rc;
}
