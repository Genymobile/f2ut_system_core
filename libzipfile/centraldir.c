#include "private.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <utils/Compat.h>

enum {
    // finding the directory
    CD_SIGNATURE = 0x06054b50,
    EOCD_LEN     = 22,        // EndOfCentralDir len, excl. comment
    MAX_COMMENT_LEN = 65535,
    MAX_EOCD_SEARCH = MAX_COMMENT_LEN + EOCD_LEN,

    // central directory entries
    ENTRY_SIGNATURE = 0x02014b50,
    ENTRY_LEN = 46,          // CentralDirEnt len, excl. var fields

    // local file header
    LFH_SIZE = 30,
};

unsigned int
read_le_int(const unsigned char* buf)
{
    return buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
}

unsigned int
read_le_short(const unsigned char* buf)
{
    return buf[0] | (buf[1] << 8);
}

static int
read_central_dir_values(Zipfile* file, const unsigned char* buf, int len)
{
    if (len < EOCD_LEN) {
        // looks like ZIP file got truncated
        fprintf(stderr, " Zip EOCD: expected >= %d bytes, found %d\n",
                EOCD_LEN, len);
        return -1;
    }

    file->disknum = read_le_short(&buf[0x04]);
    file->diskWithCentralDir = read_le_short(&buf[0x06]);
    file->entryCount = read_le_short(&buf[0x08]);
    file->totalEntryCount = read_le_short(&buf[0x0a]);
    file->centralDirSize = read_le_int(&buf[0x0c]);
    file->centralDirOffest = read_le_int(&buf[0x10]);
    file->commentLen = read_le_short(&buf[0x14]);

    if (file->commentLen > 0) {
        if (EOCD_LEN + file->commentLen > len) {
            fprintf(stderr, "EOCD(%d) + comment(%d) exceeds len (%d)\n",
                    EOCD_LEN, file->commentLen, len);
            return -1;
        }
        file->comment = buf + EOCD_LEN;
    }

    return 0;
}

static const int kCompressionStored = 0x0;
static const int kCompressionDeflate = 0x8;

static int
read_central_directory_entry(Zipfile* file, Zipentry* entry,
                const unsigned char** buf, ssize_t* len)
{
    const unsigned char* p;
    size_t remaining;
    const unsigned char* bufLimit;
    unsigned short  extraFieldLength;
    unsigned short  fileCommentLength;
    unsigned long   localHeaderRelOffset;
    unsigned int dataOffset;

    p = *buf;
    remaining = *len;
    bufLimit = file->buf + file->bufsize;

    if (*len < ENTRY_LEN) {
        fprintf(stderr, "cde entry not large enough\n");
        return -1;
    }

    if (read_le_int(&p[0x00]) != ENTRY_SIGNATURE) {
        fprintf(stderr, "Whoops: didn't find expected signature\n");
        return -1;
    }

    entry->compressionMethod = read_le_short(&p[0x0a]);
    entry->compressedSize = read_le_int(&p[0x14]);
    entry->uncompressedSize = read_le_int(&p[0x18]);
    entry->fileNameLength = read_le_short(&p[0x1c]);
    extraFieldLength = read_le_short(&p[0x1e]);
    fileCommentLength = read_le_short(&p[0x20]);
    localHeaderRelOffset = read_le_int(&p[0x2a]);

    p += ENTRY_LEN;
    remaining -= ENTRY_LEN;

    // filename
    if (entry->fileNameLength != 0) {
        if (entry->fileNameLength > remaining) {
            fprintf(stderr, "cde entry not large enough for file name.\n");
            return 1;
        }
        entry->fileName = p;
    } else {
        fprintf(stderr, "cde entry does not contain a file name.\n");
        return 1;
    }
    p += entry->fileNameLength;
    remaining -= entry->fileNameLength;
    
    // extra field, if any
    if (extraFieldLength > remaining) {
        fprintf(stderr, "cde entry not large enough for extra field.\n");
        return 1;
    }
    p += extraFieldLength;
    remaining -= extraFieldLength;

    // comment, if any
    if (fileCommentLength > remaining) {
        fprintf(stderr, "cde entry not large enough for file comment.\n");
        return 1;
    }
    p += fileCommentLength;
    remaining -= fileCommentLength;
    *buf = p;
    *len = remaining;

    // the size of the extraField in the central dir is how much data there is,
    // but the one in the local file header also contains some padding.
    p = file->buf + localHeaderRelOffset;
    if (p >= bufLimit) {
        fprintf(stderr, "Invalid local header offset for entry.\n");
        return 1;
    }
    extraFieldLength = read_le_short(&p[0x1c]);

    dataOffset = localHeaderRelOffset + LFH_SIZE
        + entry->fileNameLength + extraFieldLength;
    entry->data = file->buf + dataOffset;

    // Sanity check: make sure that the start of the entry data is within
    // our allocated buffer.
    if ((entry->data < file->buf) || (entry->data >= bufLimit)) {
        fprintf(stderr, "Invalid data offset for entry.\n");
        return 1;
    }

    // Sanity check: make sure that the end of the entry data is within
    // our allocated buffer. We need to look at the uncompressedSize for
    // stored entries and the compressed size for deflated entries.
    if ((entry->compressionMethod == kCompressionStored) &&
        (entry->uncompressedSize > (unsigned int) (bufLimit - entry->data))) {
        fprintf(stderr, "Invalid uncompressed size for stored entry.\n");
        return 1;
    }
    if ((entry->compressionMethod == kCompressionDeflate) &&
        (entry->compressedSize > (unsigned int) (bufLimit - entry->data))) {
        fprintf(stderr, "Invalid uncompressed size for deflated entry.\n");
        return 1;
    }
#if 0
    printf("file->buf=%p entry->data=%p dataOffset=%x localHeaderRelOffset=%d "
           "entry->fileNameLength=%d extraFieldLength=%d\n",
           file->buf, entry->data, dataOffset, localHeaderRelOffset,
           entry->fileNameLength, extraFieldLength);
#endif
    return 0;
}

/*
 * Find the central directory and read the contents.
 *
 * The fun thing about ZIP archives is that they may or may not be
 * readable from start to end.  In some cases, notably for archives
 * that were written to stdout, the only length information is in the
 * central directory at the end of the file.
 *
 * Of course, the central directory can be followed by a variable-length
 * comment field, so we have to scan through it backwards.  The comment
 * is at most 64K, plus we have 18 bytes for the end-of-central-dir stuff
 * itself, plus apparently sometimes people throw random junk on the end
 * just for the fun of it.
 *
 * This is all a little wobbly.  If the wrong value ends up in the EOCD
 * area, we're hosed.  This appears to be the way that everbody handles
 * it though, so we're in pretty good company if this fails.
 */
int
read_central_dir(Zipfile *file)
{
    int err;

    const unsigned char* buf = file->buf;
    ZD_TYPE bufsize = file->bufsize;
    const unsigned char* eocd;
    const unsigned char* p;
    const unsigned char* start;
    ssize_t len;
    int i;

    // too small to be a ZIP archive?
    if (bufsize < EOCD_LEN) {
        fprintf(stderr, "Length is " ZD " -- too small\n", bufsize);
        goto bail;
    }

    // find the end-of-central-dir magic
    if (bufsize > MAX_EOCD_SEARCH) {
        start = buf + bufsize - MAX_EOCD_SEARCH;
    } else {
        start = buf;
    }
    p = buf + bufsize - 4;
    while (p >= start) {
        if (*p == 0x50 && read_le_int(p) == CD_SIGNATURE) {
            eocd = p;
            break;
        }
        p--;
    }
    if (p < start) {
        fprintf(stderr, "EOCD not found, not Zip\n");
        goto bail;
    }

    // extract eocd values
    err = read_central_dir_values(file, eocd, (buf+bufsize)-eocd);
    if (err != 0) {
        goto bail;
    }

    if (file->disknum != 0
          || file->diskWithCentralDir != 0
          || file->entryCount != file->totalEntryCount) {
        fprintf(stderr, "Archive spanning not supported\n");
        goto bail;
    }

    // Loop through and read the central dir entries.
    p = buf + file->centralDirOffest;
    len = (buf+bufsize)-p;
    for (i=0; i < file->totalEntryCount; i++) {
        Zipentry* entry = malloc(sizeof(Zipentry));
        memset(entry, 0, sizeof(Zipentry));

        err = read_central_directory_entry(file, entry, &p, &len);
        if (err != 0) {
            fprintf(stderr, "read_central_directory_entry failed\n");
            free(entry);
            goto bail;
        }

        // add it to our list
        entry->next = file->entries;
        file->entries = entry;
    }

    return 0;
bail:
    return -1;
}
