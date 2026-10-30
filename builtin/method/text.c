#include <stdio.h>
#include <string.h>

#include "lizard/array.h"
#include "method/casemap.h"
#include "method/method.h"

/*
 * String methods. Text is UTF-8: lengths and positions count code points (like len) and
 * searching works on bytes (which is safe for UTF-8). Case changes use Unicode's simple
 * case mapping (one code point to one), so `ß` stays `ß` and Turkish dotless i is not
 * special. Bytes that are not valid UTF-8 pass through every method unchanged.
 */

#define LZ_TEXT_MAX_BYTES ((size_t)64 * 1024 * 1024)

static bool is_continuation(char byte) {
    return ((unsigned char)byte & 0xC0) == 0x80;
}

static const LzString *text_of(const LzValue *value) {
    return (const LzString *)value->as.object;
}

static LzStatus want_text(LzVM *vm, const char *name, const LzValue *value, const LzString **out) {
    if (value->type != LZ_VAL_OBJECT || value->as.object->type != LZ_OBJ_STRING) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "%s(): expected a string, got %s", name,
                               lz_kind_name(lz_kind_of(value)));
    }
    *out = text_of(value);
    return LZ_OK;
}

static LzStatus want_int(LzVM *vm, const char *name, const LzValue *value, int64_t *out) {
    if (value->type != LZ_VAL_INT) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "%s(): expected an int, got %s", name,
                               lz_kind_name(lz_kind_of(value)));
    }
    *out = value->as.integer;
    return LZ_OK;
}

/* Fuel: one unit per 64 bytes looked at. */
static LzStatus charge(LzVM *vm, size_t bytes) {
    return lz_vm_charge_fuel(vm, (uint64_t)bytes / 64);
}

static LzStatus make_text(LzVM *vm, const char *chars, size_t length, LzValue *result) {
    LzString *text = lz_string_new(&vm->heap, chars, length);
    if (text == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    *result = lz_value_object(&text->base);
    return LZ_OK;
}

/* A substring of text, or the text itself (retained) when nothing was cut. */
static LzStatus part_of(LzVM *vm, const LzValue *whole, size_t from, size_t to, LzValue *result) {
    const LzString *text = text_of(whole);
    if (from == 0 && to == text->length) {
        lz_value_retain(whole);
        *result = *whole;
        return LZ_OK;
    }
    return make_text(vm, text->data + from, to - from, result);
}

/* ---- UTF-8 ---- */

/* Decodes the code point at text[at]; its length in bytes, or 0 when the bytes are not valid UTF-8. */
static size_t decode(const char *text, size_t length, size_t at, uint32_t *cp) {
    unsigned char lead = (unsigned char)text[at];
    size_t size = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 0;
    if (size == 0 || at + size > length) {
        return 0;
    }
    uint32_t value = size == 1 ? lead : lead & (0xFFu >> (size + 1));
    for (size_t k = 1; k < size; k++) {
        if (!is_continuation(text[at + k])) {
            return 0;
        }
        value = (value << 6) | ((unsigned char)text[at + k] & 0x3Fu);
    }
    static const uint32_t smallest[] = {0, 0, 0x80, 0x800, 0x10000};
    if (value < smallest[size] || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
        return 0; /* overlong, too large, or a surrogate */
    }
    *cp = value;
    return size;
}

static size_t encode(uint32_t cp, char out[4]) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* ---- case ---- */

static LzStatus map_case(LzVM *vm, const char *name, const LzValue *argv, bool upper,
                         LzValue *result) {
    const LzString *text = NULL;
    LzStatus status = want_text(vm, name, &argv[0], &text);
    if (status == LZ_OK) {
        status = charge(vm, text->length);
    }
    if (status != LZ_OK) {
        return status;
    }

    LzBuffer out = {0};
    LzWriter writer = lz_writer_from_buffer(&out);
    bool ok = true;
    for (size_t at = 0; at < text->length && ok;) {
        uint32_t cp = 0;
        size_t size = decode(text->data, text->length, at, &cp);
        if (size == 0) {
            ok = lz_writer_write(&writer, text->data + at, 1); /* not UTF-8: keep the byte */
            at++;
            continue;
        }
        uint32_t mapped = upper ? lz_case_upper(cp) : lz_case_lower(cp);
        if (mapped == cp) {
            ok = lz_writer_write(&writer, text->data + at, size);
        } else {
            char bytes[4];
            ok = lz_writer_write(&writer, bytes, encode(mapped, bytes));
        }
        at += size;
    }
    if (!ok) {
        lz_buffer_free(&out);
        return lz_vm_out_of_memory(vm);
    }
    status = make_text(vm, out.data != NULL ? out.data : "", out.length, result);
    lz_buffer_free(&out);
    return status;
}

LzStatus lz_method_to_upper(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    return map_case(vm, "to_upper", argv, true, result);
}

LzStatus lz_method_to_lower(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    return map_case(vm, "to_lower", argv, false, result);
}

/* ---- trim ---- */

/* Unicode White_Space: the ASCII ones, NEL, no-break space, and the general spaces up to the ideographic one. */
static bool is_space(uint32_t cp) {
    return (cp >= 0x09 && cp <= 0x0D) || cp == 0x20 || cp == 0x85 || cp == 0xA0 || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F ||
           cp == 0x205F || cp == 0x3000;
}

/* Is the code point that ends at text[end) white space? *size is its length in bytes. */
static bool space_before(const char *text, size_t end, size_t *size) {
    size_t start = end;
    while (start > 0 && end - start < 3 && is_continuation(text[start - 1])) {
        start--;
    }
    if (start == 0) {
        *size = 0;
        return false;
    }
    start--; /* the lead byte */
    uint32_t cp = 0;
    size_t length = decode(text, end, start, &cp);
    *size = end - start;
    return length == *size && is_space(cp);
}

static LzStatus trim_with(LzVM *vm, const char *name, const LzValue *argv, bool start, bool end,
                          LzValue *result) {
    const LzString *text = NULL;
    LzStatus status = want_text(vm, name, &argv[0], &text);
    if (status != LZ_OK) {
        return status;
    }
    size_t from = 0;
    size_t to = text->length;
    while (start && from < to) {
        uint32_t cp = 0;
        size_t size = decode(text->data, to, from, &cp);
        if (size == 0 || !is_space(cp)) {
            break;
        }
        from += size;
    }
    while (end && to > from) {
        size_t size = 0;
        if (!space_before(text->data, to, &size) || to - size < from) {
            break;
        }
        to -= size;
    }
    return part_of(vm, &argv[0], from, to, result);
}

LzStatus lz_method_trim(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    return trim_with(vm, "trim", argv, true, true, result);
}

LzStatus lz_method_trim_start(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    return trim_with(vm, "trim_start", argv, true, false, result);
}

LzStatus lz_method_trim_end(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    return trim_with(vm, "trim_end", argv, false, true, result);
}

/* ---- searching ---- */

/* Byte offset of the first needle at or after from, or SIZE_MAX. */
static size_t find_bytes(const LzString *hay, const LzString *needle, size_t from) {
    if (needle->length == 0) {
        return from <= hay->length ? from : SIZE_MAX;
    }
    if (needle->length > hay->length) {
        return SIZE_MAX;
    }
    size_t last = hay->length - needle->length;
    for (size_t i = from; i <= last; i++) {
        const char *at = memchr(hay->data + i, needle->data[0], last - i + 1);
        if (at == NULL) {
            return SIZE_MAX;
        }
        i = (size_t)(at - hay->data);
        if (memcmp(at, needle->data, needle->length) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

/* Fuel for a search: the haystack once, and the needle's length makes a bad case cost more. */
static LzStatus charge_search(LzVM *vm, const LzString *hay, const LzString *needle) {
    uint64_t width = needle->length < 64 ? needle->length / 8 + 1 : 9;
    return lz_vm_charge_fuel(vm, ((uint64_t)hay->length / 64 + 1) * width);
}

static LzStatus want_two_texts(LzVM *vm, const char *name, const LzValue *argv,
                               const LzString **hay, const LzString **needle) {
    LzStatus status = want_text(vm, name, &argv[0], hay);
    if (status == LZ_OK) {
        status = want_text(vm, name, &argv[1], needle);
    }
    if (status == LZ_OK) {
        status = charge_search(vm, *hay, *needle);
    }
    return status;
}

LzStatus lz_method_contains(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    const LzString *hay = NULL;
    const LzString *needle = NULL;
    LzStatus status = want_two_texts(vm, "contains", argv, &hay, &needle);
    if (status == LZ_OK) {
        *result = lz_value_bool(find_bytes(hay, needle, 0) != SIZE_MAX);
    }
    return status;
}

LzStatus lz_method_starts_with(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    const LzString *hay = NULL;
    const LzString *prefix = NULL;
    LzStatus status = want_text(vm, "starts_with", &argv[0], &hay);
    if (status == LZ_OK) {
        status = want_text(vm, "starts_with", &argv[1], &prefix);
    }
    if (status == LZ_OK) {
        *result = lz_value_bool(prefix->length <= hay->length &&
                                memcmp(hay->data, prefix->data, prefix->length) == 0);
    }
    return status;
}

LzStatus lz_method_ends_with(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    const LzString *hay = NULL;
    const LzString *suffix = NULL;
    LzStatus status = want_text(vm, "ends_with", &argv[0], &hay);
    if (status == LZ_OK) {
        status = want_text(vm, "ends_with", &argv[1], &suffix);
    }
    if (status == LZ_OK) {
        *result = lz_value_bool(suffix->length <= hay->length &&
                                memcmp(hay->data + hay->length - suffix->length, suffix->data,
                                       suffix->length) == 0);
    }
    return status;
}

/* Code points in text[0, bytes). */
static size_t code_points(const LzString *text, size_t bytes) {
    size_t count = 0;
    for (size_t i = 0; i < bytes; i++) {
        if (!is_continuation(text->data[i])) {
            count++;
        }
    }
    return count;
}

LzStatus lz_method_index_of(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    const LzString *hay = NULL;
    const LzString *needle = NULL;
    LzStatus status = want_two_texts(vm, "index_of", argv, &hay, &needle);
    if (status != LZ_OK) {
        return status;
    }
    size_t at = find_bytes(hay, needle, 0);
    *result = lz_value_int(at == SIZE_MAX ? -1 : (int64_t)code_points(hay, at));
    return LZ_OK;
}

/* ---- building ---- */

LzStatus lz_method_replace(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    const LzString *text = NULL;
    const LzString *from = NULL;
    const LzString *to = NULL;
    LzStatus status = want_two_texts(vm, "replace", argv, &text, &from);
    if (status == LZ_OK) {
        status = want_text(vm, "replace", &argv[2], &to);
    }
    if (status != LZ_OK) {
        return status;
    }
    if (from->length == 0) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "replace(): the text to replace cannot be empty");
    }

    LzBuffer out = {0};
    LzWriter writer = lz_writer_from_buffer(&out);
    size_t position = 0;
    bool ok = true;
    for (;;) {
        size_t at = find_bytes(text, from, position);
        size_t stop = at == SIZE_MAX ? text->length : at;
        ok = ok && lz_writer_write(&writer, text->data + position, stop - position);
        if (at == SIZE_MAX) {
            break;
        }
        ok = ok && lz_writer_write(&writer, to->data, to->length);
        position = at + from->length;
        if (out.length > LZ_TEXT_MAX_BYTES) {
            lz_buffer_free(&out);
            return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "replace(): result too long (limit %zu bytes)",
                                   LZ_TEXT_MAX_BYTES);
        }
    }
    status = ok ? charge(vm, out.length) : lz_vm_out_of_memory(vm);
    if (status == LZ_OK) {
        status = make_text(vm, out.data != NULL ? out.data : "", out.length, result);
    }
    lz_buffer_free(&out);
    return status;
}

LzStatus lz_method_repeat(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    const LzString *text = NULL;
    int64_t times = 0;
    LzStatus status = want_text(vm, "repeat", &argv[0], &text);
    if (status == LZ_OK) {
        status = want_int(vm, "repeat", &argv[1], &times);
    }
    if (status != LZ_OK) {
        return status;
    }
    if (times < 0) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "repeat(): the count cannot be negative");
    }
    if (text->length != 0 && (uint64_t)times > LZ_TEXT_MAX_BYTES / text->length) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "repeat(): result too long (limit %zu bytes)",
                               LZ_TEXT_MAX_BYTES);
    }
    size_t total = text->length * (size_t)times;
    status = charge(vm, total);
    if (status != LZ_OK) {
        return status;
    }
    LzString *built = lz_heap_alloc(&vm->heap, LZ_OBJ_STRING, sizeof(LzString) + total + 1);
    if (built == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    built->length = total;
    for (size_t i = 0; i < (size_t)times; i++) {
        memcpy(built->data + i * text->length, text->data, text->length);
    }
    built->data[total] = '\0';
    built->hash = lz_hash_bytes(built->data, total);
    *result = lz_value_object(&built->base);
    return LZ_OK;
}

/* ---- pieces ---- */

LzStatus lz_method_split(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    const LzString *text = NULL;
    const LzString *separator = NULL;
    LzStatus status = want_two_texts(vm, "split", argv, &text, &separator);
    if (status != LZ_OK) {
        return status;
    }
    if (separator->length == 0) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME,
                               "split(): the separator cannot be empty (chars() splits into characters)");
    }

    size_t pieces = 1;
    for (size_t at = find_bytes(text, separator, 0); at != SIZE_MAX;
         at = find_bytes(text, separator, at + separator->length)) {
        pieces++;
    }
    if (pieces > LZ_ARRAY_MAX_LENGTH) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "split(): too many pieces (limit %zu)",
                               (size_t)LZ_ARRAY_MAX_LENGTH);
    }
    status = lz_vm_charge_fuel(vm, lz_array_cost(pieces));
    if (status != LZ_OK) {
        return status;
    }
    LzArray *array = lz_array_new(&vm->heap, pieces);
    if (array == NULL) {
        return lz_vm_out_of_memory(vm);
    }

    size_t position = 0;
    for (size_t i = 0; i < pieces && status == LZ_OK; i++) {
        size_t at = i + 1 == pieces ? SIZE_MAX : find_bytes(text, separator, position);
        size_t stop = at == SIZE_MAX ? text->length : at;
        LzValue piece;
        status = make_text(vm, text->data + position, stop - position, &piece);
        if (status == LZ_OK) {
            lz_array_note_store(array, &piece);
            array->items[array->count++] = piece; /* the new reference moves in */
        }
        position = stop + separator->length;
    }
    if (status != LZ_OK) {
        lz_object_release(&vm->heap, &array->base);
        return status;
    }
    *result = lz_value_object(&array->base);
    return LZ_OK;
}

/* Length in bytes of the code point that starts with lead. Broken UTF-8 counts as single bytes. */
static size_t sequence_length(const LzString *text, size_t at) {
    unsigned char lead = (unsigned char)text->data[at];
    size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 1;
    if (at + length > text->length) {
        return 1;
    }
    for (size_t k = 1; k < length; k++) {
        if (!is_continuation(text->data[at + k])) {
            return 1;
        }
    }
    return length;
}

LzStatus lz_method_chars(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    const LzString *text = NULL;
    LzStatus status = want_text(vm, "chars", &argv[0], &text);
    if (status == LZ_OK) {
        status = charge(vm, text->length);
    }
    if (status != LZ_OK) {
        return status;
    }
    size_t count = 0;
    for (size_t at = 0; at < text->length; at += sequence_length(text, at)) {
        count++;
    }
    if (count > LZ_ARRAY_MAX_LENGTH) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "chars(): too many characters (limit %zu)",
                               (size_t)LZ_ARRAY_MAX_LENGTH);
    }
    LzArray *array = lz_array_new(&vm->heap, count);
    if (array == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    for (size_t at = 0; at < text->length && status == LZ_OK;) {
        size_t length = sequence_length(text, at);
        LzValue piece;
        status = make_text(vm, text->data + at, length, &piece);
        if (status == LZ_OK) {
            lz_array_note_store(array, &piece);
            array->items[array->count++] = piece;
        }
        at += length;
    }
    if (status != LZ_OK) {
        lz_object_release(&vm->heap, &array->base);
        return status;
    }
    *result = lz_value_object(&array->base);
    return LZ_OK;
}

/* A bound given as nil (or left out) is open; a negative one counts from the end. */
static LzStatus resolve_bound(LzVM *vm, const LzValue *value, int64_t count, int64_t open,
                              int64_t *out) {
    if (value->type == LZ_VAL_NIL) {
        *out = open;
        return LZ_OK;
    }
    int64_t bound = 0;
    LzStatus status = want_int(vm, "slice", value, &bound);
    if (status != LZ_OK) {
        return status;
    }
    if (bound < 0) {
        bound = bound < -count ? 0 : bound + count;
    }
    *out = bound > count ? count : bound;
    return LZ_OK;
}

/* Byte offset of the code point with this index (index == count gives the end). */
static size_t byte_of(const LzString *text, int64_t index) {
    size_t at = 0;
    for (int64_t seen = 0; seen < index && at < text->length; seen++) {
        at += sequence_length(text, at);
    }
    return at;
}

LzStatus lz_method_slice(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    const LzString *text = NULL;
    LzStatus status = want_text(vm, "slice", &argv[0], &text);
    if (status == LZ_OK) {
        status = charge(vm, text->length);
    }
    if (status != LZ_OK) {
        return status;
    }
    int64_t count = (int64_t)code_points(text, text->length);
    LzValue nil = lz_value_nil();
    int64_t from = 0;
    int64_t to = count;
    status = resolve_bound(vm, &argv[1], count, 0, &from);
    if (status == LZ_OK) {
        status = resolve_bound(vm, argc > 2 ? &argv[2] : &nil, count, count, &to);
    }
    if (status != LZ_OK) {
        return status;
    }
    if (from >= to) {
        return make_text(vm, "", 0, result);
    }
    return part_of(vm, &argv[0], byte_of(text, from), byte_of(text, to), result);
}
