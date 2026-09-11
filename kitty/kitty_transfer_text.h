/*
 * kitty_transfer_text.h: every word the OSC 5113 file-transfer support shows,
 * in one place - the permission dialog, the folder picker, the Event Log
 * lines, and the status messages the far end prints (kitten shows the text
 * after the status code verbatim).
 *
 * Same rules as kitty_text.h: short, technical, edited here and nowhere else.
 */
#ifndef KITTY_TRANSFER_TEXT_H
#define KITTY_TRANSFER_TEXT_H

/* ---- the download-request dialog (files arriving; IDD_XFERDL): the request
 * text with the folder, the red warning line, Allow / Change folder... / Deny.
 * The caption is how a harness finds it. Should the template not load, the
 * same words go through the generic confirm box. ---- */
#define KT_XFER5113_CAP              "KiTTY++ File Download request"
#define KT_XFER5113_BTN_ALLOW        "&Allow"
#define KT_XFER5113_BTN_CHANGE_FOLDER "Change folder..."
#define KT_XFER5113_BTN_DENY         "&Deny"
#define KT_XFER5113_ASK_SEND         "The remote host wants to send files to this computer.\r\n\r\n" \
                                     "They will be saved in:\r\n%s"
#define KT_XFER5113_ASK_SEND_WARN    "Allow only a transfer you started yourself."
#define KT_XFER5113_ASK_RECV         "The remote host wants to read these files from this computer:\r\n\r\n%s"
#define KT_XFER5113_ASK_RECV_WARN    "The files leave this computer. Allow only a transfer you started yourself."
#define KT_XFER5113_ASK_RECV_MORE    "... and %d more"
#define KT_XFER5113_ASK_RECV_BAD     "%s  (refused: not under the upload folder)"

/* ---- the upload-request dialog (files leaving; IDD_XFERREQ): one line per
 * file with a checkbox, the count line above the list ---- */
#define KT_XFER5113_REQ_CAP          "KiTTY++ File Upload request"
#define KT_XFER5113_REQ_INTRO        "The remote host wants to read these files from this computer:"
#define KT_XFER5113_REQ_COUNT        "%d file(s), %s in total"
#define KT_XFER5113_REQ_BTN_ALLOW    "Allow &selected"
#define KT_XFER5113_REQ_BTN_DENY     "&Deny"
/* the "%s" of KT_XFER5113_REQ_COUNT */
#define KT_XFER5113_SIZE_B           "%llu bytes"
#define KT_XFER5113_SIZE_KB          "%.1f KB"
#define KT_XFER5113_SIZE_MB          "%.1f MB"
#define KT_XFER5113_SIZE_GB          "%.2f GB"

/* ---- the folder picker ("Always open Save Dialog") ---- */
#define KT_XFER5113_PICK_TITLE       "Save file to..."

/* ---- status messages sent to the far end (after the status code) ---- */
#define KT_XFER5113_ST_REFUSED       "User refused the transfer"
#define KT_XFER5113_ST_BUSY          "Another transfer is active in this window"
#define KT_XFER5113_ST_BAD_NAME      "Invalid file name"
#define KT_XFER5113_ST_DUP_FID       "The file_id already exists"
#define KT_XFER5113_ST_BAD_FTYPE     "Not a valid filetype"
#define KT_XFER5113_ST_LINKS         "Links are not supported"
#define KT_XFER5113_ST_RSYNC         "rsync transfers are not supported"
#define KT_XFER5113_ST_ISDIR         "Cannot write data to a directory entry"
#define KT_XFER5113_ST_WRITE_FAILED  "Failed to write to file"
#define KT_XFER5113_ST_CREATE_FAILED "Failed to create file"
#define KT_XFER5113_ST_MKDIR_FAILED  "Failed to create directory"
#define KT_XFER5113_ST_TOO_LARGE     "File too large"
#define KT_XFER5113_ST_TOO_MANY      "Too many files"
#define KT_XFER5113_ST_NO_SPECS      "No files requested"
#define KT_XFER5113_ST_ZLIB_CORRUPT  "Compressed data is corrupt"
#define KT_XFER5113_ST_BAD_ZIP       "Unknown compression"
#define KT_XFER5113_ST_BAD_DATA      "Invalid data encoding"
#define KT_XFER5113_ST_NOT_FOUND     "Does not exist"
#define KT_XFER5113_ST_NO_FILES      "No files found"
#define KT_XFER5113_ST_READ_FAILED   "Could not read"
#define KT_XFER5113_ST_NOT_LISTED    "Not a listed file"
#define KT_XFER5113_ST_OUTSIDE       "Not under the upload folder"
#define KT_XFER5113_ST_FULL_PATH     "Full path requests are not allowed"
#define KT_XFER5113_ST_DENIED        "Permission denied"
#define KT_XFER5113_ST_TOO_BIG_CMD   "Command too large"

/* ---- Event Log ---- */
#define KT_XFER5113_LOG_SEND_ASK     "File transfer %s: the host wants to send files to %s"
#define KT_XFER5113_LOG_SEND_OK      "File transfer %s: allowed, files go to %s"
#define KT_XFER5113_LOG_SEND_DENIED  "File transfer %s: refused by the user"
#define KT_XFER5113_LOG_SEND_AUTO    "File transfer %s: allowed by the session's permission setting"
#define KT_XFER5113_LOG_NO_DEST      "File transfer %s: no destination folder chosen, refused"
#define KT_XFER5113_LOG_BUSY         "File transfer %s: refused, another transfer is active"
#define KT_XFER5113_LOG_FILE_START   "File transfer %s: receiving %s"
#define KT_XFER5113_LOG_FILE_DONE    "File transfer %s: received %s (%llu bytes)"
#define KT_XFER5113_LOG_FILE_FAILED  "File transfer %s: %s failed (%s)"
#define KT_XFER5113_LOG_FILE_REFUSED "File transfer %s: refused \"%s\" (%s)"
#define KT_XFER5113_LOG_DIR_MADE     "File transfer %s: created folder %s"
#define KT_XFER5113_LOG_FINISHED     "File transfer %s: finished, %d file(s) received"
#define KT_XFER5113_LOG_CANCELLED    "File transfer %s: cancelled by the host"
#define KT_XFER5113_LOG_DROPPED      "File transfer %s: dropped (command out of order)"
#define KT_XFER5113_LOG_EXPIRED      "File transfer %s: expired after %d minutes without activity"
#define KT_XFER5113_LOG_RECV_ASK     "File transfer %s: the host requests to read %d path(s)"
#define KT_XFER5113_LOG_RECV_OK      "File transfer %s: allowed, %d file(s) listed"
#define KT_XFER5113_LOG_RECV_DENIED  "File transfer %s: read request refused by the user"
#define KT_XFER5113_LOG_RECV_SPEC    "File transfer %s: path \"%s\" refused (%s)"
#define KT_XFER5113_LOG_SENT         "File transfer %s: sent %s (%llu bytes)"
#define KT_XFER5113_LOG_SEND_FAILED  "File transfer %s: could not read %s"
#define KT_XFER5113_LOG_RECV_DONE    "File transfer %s: finished, %d file(s) sent"
#define KT_XFER5113_LOG_OVERSIZE     "File transfer: a command over the size limit was ignored"

#endif /* KITTY_TRANSFER_TEXT_H */
