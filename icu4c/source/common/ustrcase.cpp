// Copyright (C) 2016 and later: Unicode, Inc. and others.
// License & terms of use: http://www.unicode.org/copyright.html
/*
*******************************************************************************
*
*   Copyright (C) 2001-2015, International Business Machines
*   Corporation and others.  All Rights Reserved.
*
*******************************************************************************
*   file name:  ustrcase.cpp
*   encoding:   US-ASCII
*   tab size:   8 (not used)
*   indentation:4
*
*   created on: 2002feb20
*   created by: Markus W. Scherer
*
*   Implementation file for string casing C API functions.
*   Uses functions from uchar.c for basic functionality that requires access
*   to the Unicode Character Database (uprops.dat).
*/

#include "unicode/utypes.h"
#include "unicode/brkiter.h"
#include "unicode/ustring.h"
#include "unicode/ucasemap.h"
#include "unicode/ubrk.h"
#include "unicode/utf.h"
#include "unicode/utf16.h"
#include "cmemory.h"
#include "ucase.h"
#include "ustr_imp.h"
#include "uassert.h"

U_NAMESPACE_USE

using icu::internal::CaseMapFriend;

U_NAMESPACE_BEGIN

namespace {

// 0000uuuuuuuuuuuu records u+1 unchanged text units.
const int32_t MAX_UNCHANGED_LENGTH = 0x1000;
const int32_t MAX_UNCHANGED = MAX_UNCHANGED_LENGTH - 1;

// 0wwwcccccccccccc with w=1..6 records ccc+1 replacements of w:w text units.
// No length change.
const int32_t MAX_SHORT_WIDTH = 6;
const int32_t MAX_SHORT_CHANGE_LENGTH = 0xfff;
const int32_t MAX_SHORT_CHANGE = 0x6fff;

// 0111mmmmmmnnnnnn records a replacement of m text units with n.
// m or n = 61: actual length follows in the next edits array unit.
// m or n = 62..63: actual length follows in the next two edits array units.
// Bit 30 of the actual length is in the head unit.
// Trailing units have bit 15 set.
const int32_t LENGTH_IN_1TRAIL = 61;
const int32_t LENGTH_IN_2TRAIL = 62;

}  // namespace

Edits::~Edits() {
    if(array != stackArray) {
        uprv_free(array);
    }
}

void Edits::reset() {
    length = 0;
}

void Edits::addUnchanged(int32_t unchangedLength) {
    if(U_FAILURE(errorCode) || unchangedLength == 0) { return; }
    if(unchangedLength < 0) {
        errorCode = U_ILLEGAL_ARGUMENT_ERROR;
        return;
    }
    // Merge into previous unchanged-text record, if any.
    int32_t last = lastUnit();
    if(last < MAX_UNCHANGED) {
        int32_t remaining = MAX_UNCHANGED - last;
        if (remaining >= unchangedLength) {
            setLastUnit(last + unchangedLength);
            return;
        }
        setLastUnit(MAX_UNCHANGED);
        unchangedLength -= remaining;
    }
    // Split large lengths into multiple units.
    while(unchangedLength >= MAX_UNCHANGED_LENGTH) {
        append(MAX_UNCHANGED);
        unchangedLength -= MAX_UNCHANGED_LENGTH;
    }
    // Write a small (remaining) length.
    if(unchangedLength > 0) {
        append(unchangedLength - 1);
    }
}

void Edits::addReplace(int32_t oldLength, int32_t newLength) {
    if(U_FAILURE(errorCode)) { return; }
    if(oldLength == newLength && 0 < oldLength && oldLength <= MAX_SHORT_WIDTH) {
        // Replacement of short oldLength text units by same-length new text.
        // Merge into previous short-replacement record, if any.
        int32_t last = lastUnit();
        if(MAX_UNCHANGED < last && last < MAX_SHORT_CHANGE &&
                (last >> 12) == oldLength && (last & 0xfff) < MAX_SHORT_CHANGE_LENGTH) {
            setLastUnit(last + 1);
            return;
        }
        append(oldLength << 12);
        return;
    }

    if(oldLength < 0 || newLength < 0) {
        errorCode = U_ILLEGAL_ARGUMENT_ERROR;
        return;
    }
    if (oldLength == 0 && newLength == 0) {
        return;
    }
    int32_t newDelta = newLength - oldLength;
    if (newDelta != 0) {
        if (newDelta > 0 ? newDelta > (INT32_MAX - delta) : newDelta < (INT32_MIN - delta)) {
            // Integer overflow or underflow.
            errorCode = U_INDEX_OUTOFBOUNDS_ERROR;
            return;
        }
        delta += newDelta;
    }

    uint16_t buffer[5];
    int32_t bLength = 1;
    int32_t head = 0x7000;
    if(oldLength < LENGTH_IN_1TRAIL) {
        head |= oldLength << 6;
    } else if(oldLength <= 0x7fff) {
        head |= LENGTH_IN_1TRAIL << 6;
        buffer[bLength++] = (uint16_t)(0x8000 | oldLength);
    } else {
        head |= (LENGTH_IN_2TRAIL + (oldLength >> 30)) << 6;
        buffer[bLength++] = (uint16_t)(0x8000 | (oldLength >> 15));
        buffer[bLength++] = (uint16_t)(0x8000 | oldLength);
    }
    if(newLength < LENGTH_IN_1TRAIL) {
        head |= newLength;
    } else if(newLength <= 0x7fff) {
        head |= LENGTH_IN_1TRAIL;
        buffer[bLength++] = (uint16_t)(0x8000 | newLength);
    } else {
        head |= LENGTH_IN_2TRAIL + (newLength >> 30);
        buffer[bLength++] = (uint16_t)(0x8000 | (newLength >> 15));
        buffer[bLength++] = (uint16_t)(0x8000 | newLength);
    }
    if(bLength == 1) {
        append(head);
    } else {
        buffer[0] = (uint16_t)head;
        append(buffer, bLength);
    }
}

void Edits::append(int32_t r) {
    if(length < capacity || growArray()) {
        array[length++] = (uint16_t)r;
    }
}

void Edits::append(const uint16_t *buffer, int32_t bLength) {
    if(bLength > (INT32_MAX - length)) {
        errorCode = U_INDEX_OUTOFBOUNDS_ERROR;  // Integer overflow.
        return;
    }
    if((length + bLength) < capacity || growArray()) {
        int32_t i = 0;
        do {
            array[length++] = buffer[i++];
        } while (i < bLength);
    }
}

UBool Edits::growArray() {
    int32_t newCapacity;
    if (array == stackArray) {
        newCapacity = 2000;
    } else if (capacity == INT32_MAX) {
        errorCode = U_BUFFER_OVERFLOW_ERROR;
        return FALSE;
    } else if (capacity >= (INT32_MAX / 2)) {
        newCapacity = INT32_MAX;
    } else {
        newCapacity = 2 * capacity;
    }
    // Grow by at least 5 units so that a maximal change record will fit.
    if ((newCapacity - capacity) < 5) {
        errorCode = U_BUFFER_OVERFLOW_ERROR;
        return FALSE;
    }
    uint16_t *newArray = (uint16_t *)uprv_malloc((size_t)newCapacity * 2);
    if (newArray == NULL) {
        errorCode = U_MEMORY_ALLOCATION_ERROR;
        return FALSE;
    }
    uprv_memcpy(newArray, array, (size_t)length * 2);
    if (array != stackArray) {
        uprv_free(array);
    }
    array = newArray;
    capacity = newCapacity;
    return TRUE;
}

UBool Edits::copyErrorTo(UErrorCode &outErrorCode) {
    if (U_FAILURE(outErrorCode)) { return TRUE; }
    if (U_SUCCESS(errorCode)) { return FALSE; }
    outErrorCode = errorCode;
    return TRUE;
}

UBool Edits::hasChanges() const {
    if (delta != 0) {
        return TRUE;
    }
    for (int32_t i = 0; i < length; ++i) {
        if (array[i] > MAX_UNCHANGED) {
            return TRUE;
        }
    }
    return FALSE;
}

Edits::Iterator::Iterator(const uint16_t *a, int32_t len, UBool oc, UBool crs) :
        array(a), index(0), length(len), remaining(0),
        onlyChanges(oc), coarse(crs),
        changed(FALSE), oldLength_(0), newLength_(0),
        srcIndex(0), replIndex(0), destIndex(0) {}

int32_t Edits::Iterator::readLength(int32_t head) {
    if (head < LENGTH_IN_1TRAIL) {
        return head;
    } else if (head < LENGTH_IN_2TRAIL) {
        U_ASSERT(index < length);
        U_ASSERT(array[index] >= 0x8000);
        return array[index++];
    } else {
        U_ASSERT((index + 2) <= length);
        U_ASSERT(array[index] >= 0x8000);
        U_ASSERT(array[index + 1] >= 0x8000);
        int32_t len = ((head & 1) << 30) |
                ((int32_t)(array[index] & 0x7fff) << 15) |
                (array[index + 1] & 0x7fff);
        index += 2;
        return len;
    }
}

void Edits::Iterator::updateIndexes() {
    srcIndex += oldLength_;
    if (changed) {
        replIndex += newLength_;
    }
    destIndex += newLength_;
}

UBool Edits::Iterator::noNext() {
    // Empty span beyond the string.
    oldLength_ = newLength_ = 0;
    return FALSE;
}

UBool Edits::Iterator::next(UErrorCode &errorCode) {
    if (U_FAILURE(errorCode)) { return FALSE; }
    // We have an errorCode in case we need to start guarding against integer overflows.
    // It is also convenient for caller loops if we bail out when an error was set elsewhere.
    updateIndexes();
    if (remaining > 0) {
        // Fine-grained iterator: Continue a sequence of equal-length changes.
        --remaining;
        return TRUE;
    }
    if (index >= length) {
        return noNext();
    }
    int32_t u = array[index++];
    if (u <= MAX_UNCHANGED) {
        // Combine adjacent unchanged ranges.
        changed = FALSE;
        oldLength_ = u + 1;
        while (index < length && (u = array[index]) <= MAX_UNCHANGED) {
            ++index;
            oldLength_ += u + 1;
        }
        newLength_ = oldLength_;
        if (onlyChanges) {
            updateIndexes();
            if (index >= length) {
                return noNext();
            }
            // already fetched u > MAX_UNCHANGED at index
            ++index;
        } else {
            return TRUE;
        }
    }
    changed = TRUE;
    if (u <= MAX_SHORT_CHANGE) {
        if (coarse) {
            int32_t w = u >> 12;
            int32_t len = (u & 0xfff) + 1;
            oldLength_ = newLength_ = len * w;
        } else {
            // Split a sequence of equal-length changes that was compressed into one unit.
            oldLength_ = newLength_ = u >> 12;
            remaining = u & 0xfff;
            return TRUE;
        }
    } else {
        U_ASSERT(u <= 0x7fff);
        oldLength_ = readLength((u >> 6) & 0x3f);
        newLength_ = readLength(u & 0x3f);
        if (!coarse) {
            return TRUE;
        }
    }
    // Combine adjacent changes.
    while (index < length && (u = array[index]) > MAX_UNCHANGED) {
        ++index;
        if (u <= MAX_SHORT_CHANGE) {
            int32_t w = u >> 12;
            int32_t len = (u & 0xfff) + 1;
            len = len * w;
            oldLength_ += len;
            newLength_ += len;
        } else {
            U_ASSERT(u <= 0x7fff);
            int32_t oldLen = readLength((u >> 6) & 0x3f);
            int32_t newLen = readLength(u & 0x3f);
            oldLength_ += oldLen;
            newLength_ += newLen;
        }
    }
    return TRUE;
}

UBool Edits::Iterator::findSourceIndex(int32_t i, UErrorCode &errorCode) {
    if (U_FAILURE(errorCode) || i < 0) { return FALSE; }
    if (i < srcIndex) {
        // Reset the iterator to the start.
        index = remaining = srcIndex = replIndex = destIndex = 0;
    } else if (i < (srcIndex + oldLength_)) {
        // The index is in the current span.
        return TRUE;
    }
    while (next(errorCode)) {
        if (i < (srcIndex + oldLength_)) {
            // The index is in the current span.
            return TRUE;
        }
        if (remaining > 0) {
            // Is the index in one of the remaining compressed edits?
            // srcIndex is the start of the current span, before the remaining ones.
            int32_t len = (remaining + 1) * oldLength_;
            if (i < (srcIndex + len)) {
                int32_t n = (i - srcIndex) / oldLength_;  // 1 <= n <= remaining
                len = n * oldLength_;
                srcIndex += len;
                replIndex += len;
                destIndex += len;
                remaining -= n;
                return TRUE;
            }
            // Make next() skip all of these edits at once.
            oldLength_ = newLength_ = len;
            remaining = 0;
        }
    }
    return FALSE;
}

namespace {

int32_t checkOverflowAndEditsError(int32_t destIndex, int32_t destCapacity,
                                   Edits *edits, UErrorCode &errorCode) {
    if (U_SUCCESS(errorCode)) {
        if (destIndex > destCapacity) {
            errorCode = U_BUFFER_OVERFLOW_ERROR;
        } else if (edits != NULL) {
            edits->copyErrorTo(errorCode);
        }
    }
    return destIndex;
}

}  // namespace

U_NAMESPACE_END

U_NAMESPACE_USE

/* string casing ------------------------------------------------------------ */

/* Appends a full case mapping result, see UCASE_MAX_STRING_LENGTH. */
static inline int32_t
appendResult(UChar *dest, int32_t destIndex, int32_t destCapacity,
             int32_t result, const UChar *s,
             int32_t cpLength, icu::Edits *edits) {
    UChar32 c;
    int32_t length;

    /* decode the result */
    if(result<0) {
        /* (not) original code point */
        if(edits!=NULL) {
            edits->addUnchanged(cpLength);
            if(edits->omitUnchanged()) {
                return destIndex;
            }
        }
        c=~result;
        if(destIndex<destCapacity && c<=0xffff) {  // BMP slightly-fastpath
            dest[destIndex++]=(UChar)c;
            return destIndex;
        }
        length=cpLength;
    } else {
        if(result<=UCASE_MAX_STRING_LENGTH) {
            c=U_SENTINEL;
            length=result;
        } else if(destIndex<destCapacity && result<=0xffff) {  // BMP slightly-fastpath
            dest[destIndex++]=(UChar)result;
            if(edits!=NULL) {
                edits->addReplace(cpLength, 1);
            }
            return destIndex;
        } else {
            c=result;
            length=U16_LENGTH(c);
        }
        if(edits!=NULL) {
            edits->addReplace(cpLength, length);
        }
    }
    if(length>(INT32_MAX-destIndex)) {
        return -1;  // integer overflow
    }

    if(destIndex<destCapacity) {
        /* append the result */
        if(c>=0) {
            /* code point */
            UBool isError=FALSE;
            U16_APPEND(dest, destIndex, destCapacity, c, isError);
            if(isError) {
                /* overflow, nothing written */
                destIndex+=length;
            }
        } else {
            /* string */
            if((destIndex+length)<=destCapacity) {
                while(length>0) {
                    dest[destIndex++]=*s++;
                    --length;
                }
            } else {
                /* overflow */
                destIndex+=length;
            }
        }
    } else {
        /* preflight */
        destIndex+=length;
    }
    return destIndex;
}

static inline int32_t
appendUChar(UChar *dest, int32_t destIndex, int32_t destCapacity, UChar c) {
    if(destIndex<destCapacity) {
        dest[destIndex]=c;
    } else if(destIndex==INT32_MAX) {
        return -1;  // integer overflow
    }
    return destIndex+1;
}

static inline int32_t
appendUnchanged(UChar *dest, int32_t destIndex, int32_t destCapacity,
                const UChar *s, int32_t length, icu::Edits *edits) {
    if(length>0) {
        if(edits!=NULL) {
            edits->addUnchanged(length);
            if(edits->omitUnchanged()) {
                return destIndex;
            }
        }
        if(length>(INT32_MAX-destIndex)) {
            return -1;  // integer overflow
        }
        if((destIndex+length)<=destCapacity) {
            u_memcpy(dest+destIndex, s, length);
        }
        destIndex+=length;
    }
    return destIndex;
}

static UChar32 U_CALLCONV
utf16_caseContextIterator(void *context, int8_t dir) {
    UCaseContext *csc=(UCaseContext *)context;
    UChar32 c;

    if(dir<0) {
        /* reset for backward iteration */
        csc->index=csc->cpStart;
        csc->dir=dir;
    } else if(dir>0) {
        /* reset for forward iteration */
        csc->index=csc->cpLimit;
        csc->dir=dir;
    } else {
        /* continue current iteration direction */
        dir=csc->dir;
    }

    if(dir<0) {
        if(csc->start<csc->index) {
            U16_PREV((const UChar *)csc->p, csc->start, csc->index, c);
            return c;
        }
    } else {
        if(csc->index<csc->limit) {
            U16_NEXT((const UChar *)csc->p, csc->index, csc->limit, c);
            return c;
        }
    }
    return U_SENTINEL;
}

/*
 * Case-maps [srcStart..srcLimit[ but takes
 * context [0..srcLength[ into account.
 */
static int32_t
_caseMap(const CaseMap &csm, UCaseMapFull *map,
         UChar *dest, int32_t destCapacity,
         const UChar *src, UCaseContext *csc,
         int32_t srcStart, int32_t srcLimit,
         icu::Edits *edits,
         UErrorCode &errorCode) {
    int32_t locCache = CaseMapFriend::caseLocale(csm);

    /* case mapping loop */
    int32_t srcIndex=srcStart;
    int32_t destIndex=0;
    while(srcIndex<srcLimit) {
        int32_t cpStart;
        csc->cpStart=cpStart=srcIndex;
        UChar32 c;
        U16_NEXT(src, srcIndex, srcLimit, c);
        csc->cpLimit=srcIndex;
        const UChar *s;
        c=map(NULL, c, utf16_caseContextIterator, csc, &s, NULL, &locCache);
        destIndex = appendResult(dest, destIndex, destCapacity, c, s,
                                 srcIndex - cpStart, edits);
        if (destIndex < 0) {
            errorCode = U_INDEX_OUTOFBOUNDS_ERROR;
            return 0;
        }
    }

    return destIndex;
}

#if !UCONFIG_NO_BREAK_ITERATION

U_CFUNC int32_t U_CALLCONV
ustrcase_internalToTitle(const CaseMap &csm, BreakIterator *iter,
                         UChar *dest, int32_t destCapacity,
                         const UChar *src, int32_t srcLength,
                         icu::Edits *edits,
                         UErrorCode &errorCode) {
    if(U_FAILURE(errorCode)) {
        return 0;
    }

    /* set up local variables */
    int32_t locCache=CaseMapFriend::caseLocale(csm);
    UCaseContext csc=UCASECONTEXT_INITIALIZER;
    csc.p=(void *)src;
    csc.limit=srcLength;
    int32_t destIndex=0;
    int32_t prev=0;
    UBool isFirstIndex=TRUE;

    /* titlecasing loop */
    while(prev<srcLength) {
        /* find next index where to titlecase */
        int32_t idx;
        if(isFirstIndex) {
            isFirstIndex=FALSE;
            idx=iter->first();
        } else {
            idx=iter->next();
        }
        if(idx==UBRK_DONE || idx>srcLength) {
            idx=srcLength;
        }

        /*
         * Unicode 4 & 5 section 3.13 Default Case Operations:
         *
         * R3  toTitlecase(X): Find the word boundaries based on Unicode Standard Annex
         * #29, "Text Boundaries." Between each pair of word boundaries, find the first
         * cased character F. If F exists, map F to default_title(F); then map each
         * subsequent character C to default_lower(C).
         *
         * In this implementation, segment [prev..index[ into 3 parts:
         * a) uncased characters (copy as-is) [prev..titleStart[
         * b) first case letter (titlecase)         [titleStart..titleLimit[
         * c) subsequent characters (lowercase)                 [titleLimit..index[
         */
        if(prev<idx) {
            /* find and copy uncased characters [prev..titleStart[ */
            int32_t titleStart=prev;
            int32_t titleLimit=prev;
            UChar32 c;
            U16_NEXT(src, titleLimit, idx, c);
            if((CaseMapFriend::options(csm)&U_TITLECASE_NO_BREAK_ADJUSTMENT)==0 && UCASE_NONE==ucase_getType(NULL, c)) {
                /* Adjust the titlecasing index (titleStart) to the next cased character. */
                for(;;) {
                    titleStart=titleLimit;
                    if(titleLimit==idx) {
                        /*
                         * only uncased characters in [prev..index[
                         * stop with titleStart==titleLimit==index
                         */
                        break;
                    }
                    U16_NEXT(src, titleLimit, idx, c);
                    if(UCASE_NONE!=ucase_getType(NULL, c)) {
                        break; /* cased letter at [titleStart..titleLimit[ */
                    }
                }
                destIndex=appendUnchanged(dest, destIndex, destCapacity,
                                          src+prev, titleStart-prev, edits);
                if(destIndex<0) {
                    errorCode=U_INDEX_OUTOFBOUNDS_ERROR;
                    return 0;
                }
            }

            if(titleStart<titleLimit) {
                /* titlecase c which is from [titleStart..titleLimit[ */
                csc.cpStart=titleStart;
                csc.cpLimit=titleLimit;
                const UChar *s;
                c=ucase_toFullTitle(NULL, c, utf16_caseContextIterator, &csc, &s,
                                    NULL, &locCache);
                destIndex=appendResult(dest, destIndex, destCapacity, c, s,
                                       titleLimit-titleStart, edits);
                if(destIndex<0) {
                    errorCode=U_INDEX_OUTOFBOUNDS_ERROR;
                    return 0;
                }

                /* Special case Dutch IJ titlecasing */
                if (titleStart+1 < idx &&
                        locCache == UCASE_LOC_DUTCH &&
                        (src[titleStart] == 0x0049 || src[titleStart] == 0x0069) &&
                        (src[titleStart+1] == 0x004A || src[titleStart+1] == 0x006A)) {
                    destIndex=appendUChar(dest, destIndex, destCapacity, 0x004A);
                    if(destIndex<0) {
                        errorCode=U_INDEX_OUTOFBOUNDS_ERROR;
                        return 0;
                    }
                    if(edits!=NULL) {
                        edits->addReplace(1, 1);
                    }
                    titleLimit++;
                }

                /* lowercase [titleLimit..index[ */
                if(titleLimit<idx) {
                    if((CaseMapFriend::options(csm)&U_TITLECASE_NO_LOWERCASE)==0) {
                        /* Normal operation: Lowercase the rest of the word. */
                        destIndex+=
                            _caseMap(
                                csm, ucase_toFullLower,
                                dest+destIndex, destCapacity-destIndex,
                                src, &csc,
                                titleLimit, idx,
                                edits, errorCode);
                        if(errorCode==U_BUFFER_OVERFLOW_ERROR) {
                            errorCode=U_ZERO_ERROR;
                        }
                        if(U_FAILURE(errorCode)) {
                            return destIndex;
                        }
                    } else {
                        /* Optionally just copy the rest of the word unchanged. */
                        destIndex=appendUnchanged(dest, destIndex, destCapacity,
                                                  src+titleLimit, idx-titleLimit, edits);
                        if(destIndex<0) {
                            errorCode=U_INDEX_OUTOFBOUNDS_ERROR;
                            return 0;
                        }
                    }
                }
            }
        }

        prev=idx;
    }

    return checkOverflowAndEditsError(destIndex, destCapacity, edits, errorCode);
}

#endif  // !UCONFIG_NO_BREAK_ITERATION

U_NAMESPACE_BEGIN
namespace GreekUpper {

// Data generated by prototype code, see
// http://site.icu-project.org/design/case/greek-upper
// TODO: Move this data into ucase.icu.
static const uint16_t data0370[] = {
    // U+0370..03FF
    0x0370,
    0x0370,
    0x0372,
    0x0372,
    0,
    0,
    0x0376,
    0x0376,
    0,
    0,
    0x037A,
    0x03FD,
    0x03FE,
    0x03FF,
    0,
    0x037F,
    0,
    0,
    0,
    0,
    0,
    0,
    0x0391 | HAS_VOWEL | HAS_ACCENT,
    0,
    0x0395 | HAS_VOWEL | HAS_ACCENT,
    0x0397 | HAS_VOWEL | HAS_ACCENT,
    0x0399 | HAS_VOWEL | HAS_ACCENT,
    0,
    0x039F | HAS_VOWEL | HAS_ACCENT,
    0,
    0x03A5 | HAS_VOWEL | HAS_ACCENT,
    0x03A9 | HAS_VOWEL | HAS_ACCENT,
    0x0399 | HAS_VOWEL | HAS_ACCENT | HAS_DIALYTIKA,
    0x0391 | HAS_VOWEL,
    0x0392,
    0x0393,
    0x0394,
    0x0395 | HAS_VOWEL,
    0x0396,
    0x0397 | HAS_VOWEL,
    0x0398,
    0x0399 | HAS_VOWEL,
    0x039A,
    0x039B,
    0x039C,
    0x039D,
    0x039E,
    0x039F | HAS_VOWEL,
    0x03A0,
    0x03A1,
    0,
    0x03A3,
    0x03A4,
    0x03A5 | HAS_VOWEL,
    0x03A6,
    0x03A7,
    0x03A8,
    0x03A9 | HAS_VOWEL,
    0x0399 | HAS_VOWEL | HAS_DIALYTIKA,
    0x03A5 | HAS_VOWEL | HAS_DIALYTIKA,
    0x0391 | HAS_VOWEL | HAS_ACCENT,
    0x0395 | HAS_VOWEL | HAS_ACCENT,
    0x0397 | HAS_VOWEL | HAS_ACCENT,
    0x0399 | HAS_VOWEL | HAS_ACCENT,
    0x03A5 | HAS_VOWEL | HAS_ACCENT | HAS_DIALYTIKA,
    0x0391 | HAS_VOWEL,
    0x0392,
    0x0393,
    0x0394,
    0x0395 | HAS_VOWEL,
    0x0396,
    0x0397 | HAS_VOWEL,
    0x0398,
    0x0399 | HAS_VOWEL,
    0x039A,
    0x039B,
    0x039C,
    0x039D,
    0x039E,
    0x039F | HAS_VOWEL,
    0x03A0,
    0x03A1,
    0x03A3,
    0x03A3,
    0x03A4,
    0x03A5 | HAS_VOWEL,
    0x03A6,
    0x03A7,
    0x03A8,
    0x03A9 | HAS_VOWEL,
    0x0399 | HAS_VOWEL | HAS_DIALYTIKA,
    0x03A5 | HAS_VOWEL | HAS_DIALYTIKA,
    0x039F | HAS_VOWEL | HAS_ACCENT,
    0x03A5 | HAS_VOWEL | HAS_ACCENT,
    0x03A9 | HAS_VOWEL | HAS_ACCENT,
    0x03CF,
    0x0392,
    0x0398,
    0x03D2,
    0x03D2 | HAS_ACCENT,
    0x03D2 | HAS_DIALYTIKA,
    0x03A6,
    0x03A0,
    0x03CF,
    0x03D8,
    0x03D8,
    0x03DA,
    0x03DA,
    0x03DC,
    0x03DC,
    0x03DE,
    0x03DE,
    0x03E0,
    0x03E0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0x039A,
    0x03A1,
    0x03F9,
    0x037F,
    0x03F4,
    0x0395 | HAS_VOWEL,
    0,
    0x03F7,
    0x03F7,
    0x03F9,
    0x03FA,
    0x03FA,
    0x03FC,
    0x03FD,
    0x03FE,
    0x03FF,
};

static const uint16_t data1F00[] = {
    // U+1F00..1FFF
    0x0391 | HAS_VOWEL,
    0x0391 | HAS_VOWEL,
    0x0391 | HAS_VOWEL | HAS_ACCENT,
    0x0391 | HAS_VOWEL | HAS_ACCENT,
    0x0391 | HAS_VOWEL | HAS_ACCENT,
    0x0391 | HAS_VOWEL | HAS_ACCENT,
    0x0391 | HAS_VOWEL | HAS_ACCENT,
    0x0391 | HAS_VOWEL | HAS_ACCENT,
    0x0391 | HAS_VOWEL,
    0x0391 | HAS_VOWEL,
    0x0391 | HAS_VOWEL | HAS_ACCENT,
    0x0391 | HAS_VOWEL | HAS_ACCENT,
    0x0391 | HAS_VOWEL | HAS_ACCENT,
    0x0391 | HAS_VOWEL | HAS_ACCENT,
    0x0391 | HAS_VOWEL | HAS_ACCENT,
    0x0391 | HAS_VOWEL | HAS_ACCENT,
    0x0395 | HAS_VOWEL,
    0x0395 | HAS_VOWEL,
    0x0395 | HAS_VOWEL | HAS_ACCENT,
    0x0395 | HAS_VOWEL | HAS_ACCENT,
    0x0395 | HAS_VOWEL | HAS_ACCENT,
    0x0395 | HAS_VOWEL | HAS_ACCENT,
    0,
    0,
    0x0395 | HAS_VOWEL,
    0x0395 | HAS_VOWEL,
    0x0395 | HAS_VOWEL | HAS_ACCENT,
    0x0395 | HAS_VOWEL | HAS_ACCENT,
    0x0395 | HAS_VOWEL | HAS_ACCENT,
    0x0395 | HAS_VOWEL | HAS_ACCENT,
    0,
    0,
    0x0397 | HAS_VOWEL,
    0x0397 | HAS_VOWEL,
    0x0397 | HAS_VOWEL | HAS_ACCENT,
    0x0397 | HAS_VOWEL | HAS_ACCENT,
    0x0397 | HAS_VOWEL | HAS_ACCENT,
    0x0397 | HAS_VOWEL | HAS_ACCENT,
    0x0397 | HAS_VOWEL | HAS_ACCENT,
    0x0397 | HAS_VOWEL | HAS_ACCENT,
    0x0397 | HAS_VOWEL,
    0x0397 | HAS_VOWEL,
    0x0397 | HAS_VOWEL | HAS_ACCENT,
    0x0397 | HAS_VOWEL | HAS_ACCENT,
    0x0397 | HAS_VOWEL | HAS_ACCENT,
    0x0397 | HAS_VOWEL | HAS_ACCENT,
    0x0397 | HAS_VOWEL | HAS_ACCENT,
    0x0397 | HAS_VOWEL | HAS_ACCENT,
    0x0399 | HAS_VOWEL,
    0x0399 | HAS_VOWEL,
    0x0399 | HAS_VOWEL | HAS_ACCENT,
    0x0399 | HAS_VOWEL | HAS_ACCENT,
    0x0399 | HAS_VOWEL | HAS_ACCENT,
    0x0399 | HAS_VOWEL | HAS_ACCENT,
    0x0399 | HAS_VOWEL | HAS_ACCENT,
    0x0399 | HAS_VOWEL | HAS_ACCENT,
    0x0399 | HAS_VOWEL,
    0x0399 | HAS_VOWEL,
    0x0399 | HAS_VOWEL | HAS_ACCENT,
    0x0399 | HAS_VOWEL | HAS_ACCENT,
    0x0399 | HAS_VOWEL | HAS_ACCENT,
    0x0399 | HAS_VOWEL | HAS_ACCENT,
    0x0399 | HAS_VOWEL | HAS_ACCENT,
    0x0399 | HAS_VOWEL | HAS_ACCENT,
    0x039F | HAS_VOWEL,
    0x039F | HAS_VOWEL,
    0x039F | HAS_VOWEL | HAS_ACCENT,
    0x039F | HAS_VOWEL | HAS_ACCENT,
    0x039F | HAS_VOWEL | HAS_ACCENT,
    0x039F | HAS_VOWEL | HAS_ACCENT,
    0,
    0,
    0x039F | HAS_VOWEL,
    0x039F | HAS_VOWEL,
    0x039F | HAS_VOWEL | HAS_ACCENT,
    0x039F | HAS_VOWEL | HAS_ACCENT,
    0x039F | HAS_VOWEL | HAS_ACCENT,
    0x039F | HAS_VOWEL | HAS_ACCENT,
    0,
    0,
    0x03A5 | HAS_VOWEL,
    0x03A5 | HAS_VOWEL,
    0x03A5 | HAS_VOWEL | HAS_ACCENT,
    0x03A5 | HAS_VOWEL | HAS_ACCENT,
    0x03A5 | HAS_VOWEL | HAS_ACCENT,
    0x03A5 | HAS_VOWEL | HAS_ACCENT,
    0x03A5 | HAS_VOWEL | HAS_ACCENT,
    0x03A5 | HAS_VOWEL | HAS_ACCENT,
    0,
    0x03A5 | HAS_VOWEL,
    0,
    0x03A5 | HAS_VOWEL | HAS_ACCENT,
    0,
    0x03A5 | HAS_VOWEL | HAS_ACCENT,
    0,
    0x03A5 | HAS_VOWEL | HAS_ACCENT,
    0x03A9 | HAS_VOWEL,
    0x03A9 | HAS_VOWEL,
    0x03A9 | HAS_VOWEL | HAS_ACCENT,
    0x03A9 | HAS_VOWEL | HAS_ACCENT,
    0x03A9 | HAS_VOWEL | HAS_ACCENT,
    0x03A9 | HAS_VOWEL | HAS_ACCENT,
    0x03A9 | HAS_VOWEL | HAS_ACCENT,
    0x03A9 | HAS_VOWEL | HAS_ACCENT,
    0x03A9 | HAS_VOWEL,
    0x03A9 | HAS_VOWEL,
    0x03A9 | HAS_VOWEL | HAS_ACCENT,
    0x03A9 | HAS_VOWEL | HAS_ACCENT,
    0x03A9 | HAS_VOWEL | HAS_ACCENT,
    0x03A9 | HAS_VOWEL | HAS_ACCENT,
    0x03A9 | HAS_VOWEL | HAS_ACCENT,
    0x03A9 | HAS_VOWEL | HAS_ACCENT,
    0x0391 | HAS_VOWEL | HAS_ACCENT,
    0x0391 | HAS_VOWEL | HAS_ACCENT,
    0x0395 | HAS_VOWEL | HAS_ACCENT,
    0x0395 | HAS_VOWEL | HAS_ACCENT,
    0x0397 | HAS_VOWEL | HAS_ACCENT,
    0x0397 | HAS_VOWEL | HAS_ACCENT,
    0x0399 | HAS_VOWEL | HAS_ACCENT,
    0x0399 | HAS_VOWEL | HAS_ACCENT,
    0x039F | HAS_VOWEL | HAS_ACCENT,
    0x039F | HAS_VOWEL | HAS_ACCENT,
    0x03A5 | HAS_VOWEL | HAS_ACCENT,
    0x03A5 | HAS_VOWEL | HAS_ACCENT,
    0x03A9 | HAS_VOWEL | HAS_ACCENT,
    0x03A9 | HAS_VOWEL | HAS_ACCENT,
    0,
    0,
    0x0391 | HAS_VOWEL | HAS_YPOGEGRAMMENI,
    0x0391 | HAS_VOWEL | HAS_YPOGEGRAMMENI,
    0x0391 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x0391 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x0391 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x0391 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x0391 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x0391 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x0391 | HAS_VOWEL | HAS_YPOGEGRAMMENI,
    0x0391 | HAS_VOWEL | HAS_YPOGEGRAMMENI,
    0x0391 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x0391 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x0391 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x0391 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x0391 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x0391 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x0397 | HAS_VOWEL | HAS_YPOGEGRAMMENI,
    0x0397 | HAS_VOWEL | HAS_YPOGEGRAMMENI,
    0x0397 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x0397 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x0397 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x0397 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x0397 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x0397 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x0397 | HAS_VOWEL | HAS_YPOGEGRAMMENI,
    0x0397 | HAS_VOWEL | HAS_YPOGEGRAMMENI,
    0x0397 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x0397 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x0397 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x0397 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x0397 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x0397 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x03A9 | HAS_VOWEL | HAS_YPOGEGRAMMENI,
    0x03A9 | HAS_VOWEL | HAS_YPOGEGRAMMENI,
    0x03A9 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x03A9 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x03A9 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x03A9 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x03A9 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x03A9 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x03A9 | HAS_VOWEL | HAS_YPOGEGRAMMENI,
    0x03A9 | HAS_VOWEL | HAS_YPOGEGRAMMENI,
    0x03A9 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x03A9 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x03A9 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x03A9 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x03A9 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x03A9 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x0391 | HAS_VOWEL,
    0x0391 | HAS_VOWEL,
    0x0391 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x0391 | HAS_VOWEL | HAS_YPOGEGRAMMENI,
    0x0391 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0,
    0x0391 | HAS_VOWEL | HAS_ACCENT,
    0x0391 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x0391 | HAS_VOWEL,
    0x0391 | HAS_VOWEL,
    0x0391 | HAS_VOWEL | HAS_ACCENT,
    0x0391 | HAS_VOWEL | HAS_ACCENT,
    0x0391 | HAS_VOWEL | HAS_YPOGEGRAMMENI,
    0,
    0x0399 | HAS_VOWEL,
    0,
    0,
    0,
    0x0397 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x0397 | HAS_VOWEL | HAS_YPOGEGRAMMENI,
    0x0397 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0,
    0x0397 | HAS_VOWEL | HAS_ACCENT,
    0x0397 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x0395 | HAS_VOWEL | HAS_ACCENT,
    0x0395 | HAS_VOWEL | HAS_ACCENT,
    0x0397 | HAS_VOWEL | HAS_ACCENT,
    0x0397 | HAS_VOWEL | HAS_ACCENT,
    0x0397 | HAS_VOWEL | HAS_YPOGEGRAMMENI,
    0,
    0,
    0,
    0x0399 | HAS_VOWEL,
    0x0399 | HAS_VOWEL,
    0x0399 | HAS_VOWEL | HAS_ACCENT | HAS_DIALYTIKA,
    0x0399 | HAS_VOWEL | HAS_ACCENT | HAS_DIALYTIKA,
    0,
    0,
    0x0399 | HAS_VOWEL | HAS_ACCENT,
    0x0399 | HAS_VOWEL | HAS_ACCENT | HAS_DIALYTIKA,
    0x0399 | HAS_VOWEL,
    0x0399 | HAS_VOWEL,
    0x0399 | HAS_VOWEL | HAS_ACCENT,
    0x0399 | HAS_VOWEL | HAS_ACCENT,
    0,
    0,
    0,
    0,
    0x03A5 | HAS_VOWEL,
    0x03A5 | HAS_VOWEL,
    0x03A5 | HAS_VOWEL | HAS_ACCENT | HAS_DIALYTIKA,
    0x03A5 | HAS_VOWEL | HAS_ACCENT | HAS_DIALYTIKA,
    0x03A1,
    0x03A1,
    0x03A5 | HAS_VOWEL | HAS_ACCENT,
    0x03A5 | HAS_VOWEL | HAS_ACCENT | HAS_DIALYTIKA,
    0x03A5 | HAS_VOWEL,
    0x03A5 | HAS_VOWEL,
    0x03A5 | HAS_VOWEL | HAS_ACCENT,
    0x03A5 | HAS_VOWEL | HAS_ACCENT,
    0x03A1,
    0,
    0,
    0,
    0,
    0,
    0x03A9 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x03A9 | HAS_VOWEL | HAS_YPOGEGRAMMENI,
    0x03A9 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0,
    0x03A9 | HAS_VOWEL | HAS_ACCENT,
    0x03A9 | HAS_VOWEL | HAS_YPOGEGRAMMENI | HAS_ACCENT,
    0x039F | HAS_VOWEL | HAS_ACCENT,
    0x039F | HAS_VOWEL | HAS_ACCENT,
    0x03A9 | HAS_VOWEL | HAS_ACCENT,
    0x03A9 | HAS_VOWEL | HAS_ACCENT,
    0x03A9 | HAS_VOWEL | HAS_YPOGEGRAMMENI,
    0,
    0,
    0,
};

// U+2126 Ohm sign
static const uint16_t data2126 = 0x03A9 | HAS_VOWEL;

uint32_t getLetterData(UChar32 c) {
    if (c < 0x370 || 0x2126 < c || (0x3ff < c && c < 0x1f00)) {
        return 0;
    } else if (c <= 0x3ff) {
        return data0370[c - 0x370];
    } else if (c <= 0x1fff) {
        return data1F00[c - 0x1f00];
    } else if (c == 0x2126) {
        return data2126;
    } else {
        return 0;
    }
}

uint32_t getDiacriticData(UChar32 c) {
    switch (c) {
    case 0x0300:  // varia
    case 0x0301:  // tonos = oxia
    case 0x0342:  // perispomeni
    case 0x0302:  // circumflex can look like perispomeni
    case 0x0303:  // tilde can look like perispomeni
    case 0x0311:  // inverted breve can look like perispomeni
        return HAS_ACCENT;
    case 0x0308:  // dialytika = diaeresis
        return HAS_COMBINING_DIALYTIKA;
    case 0x0344:  // dialytika tonos
        return HAS_COMBINING_DIALYTIKA | HAS_ACCENT;
    case 0x0345:  // ypogegrammeni = iota subscript
        return HAS_YPOGEGRAMMENI;
    case 0x0304:  // macron
    case 0x0306:  // breve
    case 0x0313:  // comma above
    case 0x0314:  // reversed comma above
    case 0x0343:  // koronis
        return HAS_OTHER_GREEK_DIACRITIC;
    default:
        return 0;
    }
}

UBool isFollowedByCasedLetter(const UCaseProps *csp, const UChar *s, int32_t i, int32_t length) {
    while (i < length) {
        UChar32 c;
        U16_NEXT(s, i, length, c);
        int32_t type = ucase_getTypeOrIgnorable(csp, c);
        if ((type & UCASE_IGNORABLE) != 0) {
            // Case-ignorable, continue with the loop.
        } else if (type != UCASE_NONE) {
            return TRUE;  // Followed by cased letter.
        } else {
            return FALSE;  // Uncased and not case-ignorable.
        }
    }
    return FALSE;  // Not followed by cased letter.
}

/**
 * Greek string uppercasing with a state machine.
 * Probably simpler than a stateless function that has to figure out complex context-before
 * for each character.
 * TODO: Try to re-consolidate one way or another with the non-Greek function.
 */
int32_t toUpper(const CaseMap & /* unused csm */,
                UChar *dest, int32_t destCapacity,
                const UChar *src, int32_t srcLength,
                Edits *edits,
                UErrorCode &errorCode) {
    int32_t locCache = UCASE_LOC_GREEK;
    int32_t destIndex=0;
    uint32_t state = 0;
    for (int32_t i = 0; i < srcLength;) {
        int32_t nextIndex = i;
        UChar32 c;
        U16_NEXT(src, nextIndex, srcLength, c);
        uint32_t nextState = 0;
        int32_t type = ucase_getTypeOrIgnorable(NULL, c);
        if ((type & UCASE_IGNORABLE) != 0) {
            // c is case-ignorable
            nextState |= (state & AFTER_CASED);
        } else if (type != UCASE_NONE) {
            // c is cased
            nextState |= AFTER_CASED;
        }
        uint32_t data = getLetterData(c);
        if (data > 0) {
            uint32_t upper = data & UPPER_MASK;
            // Add a dialytika to this iota or ypsilon vowel
            // if we removed a tonos from the previous vowel,
            // and that previous vowel did not also have (or gain) a dialytika.
            // Adding one only to the final vowel in a longer sequence
            // (which does not occur in normal writing) would require lookahead.
            // Set the same flag as for preserving an existing dialytika.
            if ((data & HAS_VOWEL) != 0 && (state & AFTER_VOWEL_WITH_ACCENT) != 0 &&
                    (upper == 0x399 || upper == 0x3A5)) {
                data |= HAS_DIALYTIKA;
            }
            int32_t numYpogegrammeni = 0;  // Map each one to a trailing, spacing, capital iota.
            if ((data & HAS_YPOGEGRAMMENI) != 0) {
                numYpogegrammeni = 1;
            }
            // Skip combining diacritics after this Greek letter.
            while (nextIndex < srcLength) {
                uint32_t diacriticData = getDiacriticData(src[nextIndex]);
                if (diacriticData != 0) {
                    data |= diacriticData;
                    if ((diacriticData & HAS_YPOGEGRAMMENI) != 0) {
                        ++numYpogegrammeni;
                    }
                    ++nextIndex;
                } else {
                    break;  // not a Greek diacritic
                }
            }
            if ((data & HAS_VOWEL_AND_ACCENT_AND_DIALYTIKA) == HAS_VOWEL_AND_ACCENT) {
                nextState |= AFTER_VOWEL_WITH_ACCENT;
            }
            // Map according to Greek rules.
            UBool addTonos = FALSE;
            if (upper == 0x397 &&
                    (data & HAS_ACCENT) != 0 &&
                    numYpogegrammeni == 0 &&
                    (state & AFTER_CASED) == 0 &&
                    !isFollowedByCasedLetter(NULL, src, nextIndex, srcLength)) {
                // Keep disjunctive "or" with (only) a tonos.
                // We use the same "word boundary" conditions as for the Final_Sigma test.
                if (i == nextIndex) {
                    upper = 0x389;  // Preserve the precomposed form.
                } else {
                    addTonos = TRUE;
                }
            } else if ((data & HAS_DIALYTIKA) != 0) {
                // Preserve a vowel with dialytika in precomposed form if it exists.
                if (upper == 0x399) {
                    upper = 0x3AA;
                    data &= ~HAS_EITHER_DIALYTIKA;
                } else if (upper == 0x3A5) {
                    upper = 0x3AB;
                    data &= ~HAS_EITHER_DIALYTIKA;
                }
            }

            UBool change;
            if (edits == NULL) {
                change = TRUE;  // common, simple usage
            } else {
                // Find out first whether we are changing the text.
                change = src[i] != upper || numYpogegrammeni > 0;
                int32_t i2 = i + 1;
                if ((data & HAS_EITHER_DIALYTIKA) != 0) {
                    change |= i2 >= nextIndex || src[i2] != 0x308;
                    ++i2;
                }
                if (addTonos) {
                    change |= i2 >= nextIndex || src[i2] != 0x301;
                    ++i2;
                }
                int32_t oldLength = nextIndex - i;
                int32_t newLength = (i2 - i) + numYpogegrammeni;
                change |= oldLength != newLength;
                if (change) {
                    if (edits != NULL) {
                        edits->addReplace(oldLength, newLength);
                    }
                } else {
                    if (edits != NULL) {
                        edits->addUnchanged(oldLength);
                    }
                    // Write unchanged text?
                    change = edits->writeUnchanged();
                }
            }

            if (change) {
                destIndex=appendUChar(dest, destIndex, destCapacity, (UChar)upper);
                if (destIndex >= 0 && (data & HAS_EITHER_DIALYTIKA) != 0) {
                    destIndex=appendUChar(dest, destIndex, destCapacity, 0x308);  // restore or add a dialytika
                }
                if (destIndex >= 0 && addTonos) {
                    destIndex=appendUChar(dest, destIndex, destCapacity, 0x301);
                }
                while (destIndex >= 0 && numYpogegrammeni > 0) {
                    destIndex=appendUChar(dest, destIndex, destCapacity, 0x399);
                    --numYpogegrammeni;
                }
                if(destIndex<0) {
                    errorCode=U_INDEX_OUTOFBOUNDS_ERROR;
                    return 0;
                }
            }
        } else {
            const UChar *s;
            c=ucase_toFullUpper(NULL, c, NULL, NULL, &s, NULL, &locCache);
            destIndex = appendResult(dest, destIndex, destCapacity, c, s,
                                     nextIndex - i, edits);
            if (destIndex < 0) {
                errorCode = U_INDEX_OUTOFBOUNDS_ERROR;
                return 0;
            }
        }
        i = nextIndex;
        state = nextState;
    }

    return checkOverflowAndEditsError(destIndex, destCapacity, edits, errorCode);
}

}  // namespace GreekUpper
U_NAMESPACE_END

/* functions available in the common library (for unistr_case.cpp) */

U_CFUNC int32_t U_CALLCONV
ustrcase_internalToLower(const CaseMap &csm, UCASEMAP_BREAK_ITERATOR_UNUSED
                         UChar *dest, int32_t destCapacity,
                         const UChar *src, int32_t srcLength,
                         icu::Edits *edits,
                         UErrorCode &errorCode) {
    UCaseContext csc=UCASECONTEXT_INITIALIZER;
    csc.p=(void *)src;
    csc.limit=srcLength;
    int32_t destIndex = _caseMap(
        csm, ucase_toFullLower,
        dest, destCapacity,
        src, &csc, 0, srcLength,
        edits, errorCode);
    return checkOverflowAndEditsError(destIndex, destCapacity, edits, errorCode);
}

U_CFUNC int32_t U_CALLCONV
ustrcase_internalToUpper(const CaseMap &csm, UCASEMAP_BREAK_ITERATOR_UNUSED
                         UChar *dest, int32_t destCapacity,
                         const UChar *src, int32_t srcLength,
                         icu::Edits *edits,
                         UErrorCode &errorCode) {
    int32_t locCache = CaseMapFriend::caseLocale(csm);
    if (locCache == UCASE_LOC_GREEK) {
        return GreekUpper::toUpper(csm, dest, destCapacity, src, srcLength, edits, errorCode);
    }
    UCaseContext csc=UCASECONTEXT_INITIALIZER;
    csc.p=(void *)src;
    csc.limit=srcLength;
    int32_t destIndex = _caseMap(
        csm, ucase_toFullUpper,
        dest, destCapacity,
        src, &csc, 0, srcLength,
        edits, errorCode);
    return checkOverflowAndEditsError(destIndex, destCapacity, edits, errorCode);
}

U_CFUNC int32_t U_CALLCONV
ustrcase_internalFold(const CaseMap &csm, UCASEMAP_BREAK_ITERATOR_UNUSED
                      UChar *dest, int32_t destCapacity,
                      const UChar *src, int32_t srcLength,
                      icu::Edits *edits,
                      UErrorCode &errorCode) {
    /* case mapping loop */
    int32_t srcIndex = 0;
    int32_t destIndex = 0;
    while (srcIndex < srcLength) {
        int32_t cpStart = srcIndex;
        UChar32 c;
        U16_NEXT(src, srcIndex, srcLength, c);
        const UChar *s;
        c = ucase_toFullFolding(NULL, c, &s, CaseMapFriend::options(csm));
        destIndex = appendResult(dest, destIndex, destCapacity, c, s,
                                 srcIndex - cpStart, edits);
        if (destIndex < 0) {
            errorCode = U_INDEX_OUTOFBOUNDS_ERROR;
            return 0;
        }
    }

    return checkOverflowAndEditsError(destIndex, destCapacity, edits, errorCode);
}

U_CFUNC int32_t
ustrcase_map(const CaseMap &csm, UCASEMAP_BREAK_ITERATOR_PARAM
             UChar *dest, int32_t destCapacity,
             const UChar *src, int32_t srcLength,
             UStringCaseMapper *stringCaseMapper,
             icu::Edits *edits,
             UErrorCode &errorCode) {
    int32_t destLength;

    /* check argument values */
    if(U_FAILURE(errorCode)) {
        return 0;
    }
    if( destCapacity<0 ||
        (dest==NULL && destCapacity>0) ||
        src==NULL ||
        srcLength<-1
    ) {
        errorCode=U_ILLEGAL_ARGUMENT_ERROR;
        return 0;
    }

    /* get the string length */
    if(srcLength==-1) {
        srcLength=u_strlen(src);
    }

    /* check for overlapping source and destination */
    if( dest!=NULL &&
        ((src>=dest && src<(dest+destCapacity)) ||
         (dest>=src && dest<(src+srcLength)))
    ) {
        errorCode=U_ILLEGAL_ARGUMENT_ERROR;
        return 0;
    }

    destLength=stringCaseMapper(csm, UCASEMAP_BREAK_ITERATOR
                                dest, destCapacity, src, srcLength, edits, errorCode);
    return u_terminateUChars(dest, destCapacity, destLength, &errorCode);
}

U_CFUNC int32_t
ustrcase_mapWithOverlap(const CaseMap &csm, UCASEMAP_BREAK_ITERATOR_PARAM
                        UChar *dest, int32_t destCapacity,
                        const UChar *src, int32_t srcLength,
                        UStringCaseMapper *stringCaseMapper,
                        UErrorCode &errorCode) {
    UChar buffer[300];
    UChar *temp;

    int32_t destLength;

    /* check argument values */
    if(U_FAILURE(errorCode)) {
        return 0;
    }
    if( destCapacity<0 ||
        (dest==NULL && destCapacity>0) ||
        src==NULL ||
        srcLength<-1
    ) {
        errorCode=U_ILLEGAL_ARGUMENT_ERROR;
        return 0;
    }

    /* get the string length */
    if(srcLength==-1) {
        srcLength=u_strlen(src);
    }

    /* check for overlapping source and destination */
    if( dest!=NULL &&
        ((src>=dest && src<(dest+destCapacity)) ||
         (dest>=src && dest<(src+srcLength)))
    ) {
        /* overlap: provide a temporary destination buffer and later copy the result */
        if(destCapacity<=UPRV_LENGTHOF(buffer)) {
            /* the stack buffer is large enough */
            temp=buffer;
        } else {
            /* allocate a buffer */
            temp=(UChar *)uprv_malloc(destCapacity*U_SIZEOF_UCHAR);
            if(temp==NULL) {
                errorCode=U_MEMORY_ALLOCATION_ERROR;
                return 0;
            }
        }
    } else {
        temp=dest;
    }

    destLength=stringCaseMapper(csm, UCASEMAP_BREAK_ITERATOR
                                temp, destCapacity, src, srcLength, NULL, errorCode);
    if(temp!=dest) {
        /* copy the result string to the destination buffer */
        if (U_SUCCESS(errorCode) && 0 < destLength && destLength <= destCapacity) {
            u_memmove(dest, temp, destLength);
        }
        if(temp!=buffer) {
            uprv_free(temp);
        }
    }

    return u_terminateUChars(dest, destCapacity, destLength, &errorCode);
}

/* public API functions */

U_CAPI int32_t U_EXPORT2
u_strFoldCase(UChar *dest, int32_t destCapacity,
              const UChar *src, int32_t srcLength,
              uint32_t options,
              UErrorCode *pErrorCode) {
    return ustrcase_mapWithOverlap(
        CaseMap(options, *pErrorCode), UCASEMAP_BREAK_ITERATOR_NULL
        dest, destCapacity,
        src, srcLength,
        ustrcase_internalFold, *pErrorCode);
}

U_NAMESPACE_BEGIN

int32_t CaseMap::toLower(UChar *dest, int32_t destCapacity,
                         const UChar *src, int32_t srcLength,
                         icu::Edits *edits,
                         UErrorCode &errorCode) const {
    return ustrcase_map(
        *this, UCASEMAP_BREAK_ITERATOR_NULL
        dest, destCapacity,
        src, srcLength,
        ustrcase_internalToLower, edits, errorCode);
}

int32_t CaseMap::toUpper(UChar *dest, int32_t destCapacity,
                         const UChar *src, int32_t srcLength,
                         icu::Edits *edits,
                         UErrorCode &errorCode) const {
    return ustrcase_map(
        *this, UCASEMAP_BREAK_ITERATOR_NULL
        dest, destCapacity,
        src, srcLength,
        ustrcase_internalToUpper, edits, errorCode);
}

int32_t CaseMap::foldCase(UChar *dest, int32_t destCapacity,
                          const UChar *src, int32_t srcLength,
                          icu::Edits *edits,
                          UErrorCode &errorCode) const {
    return ustrcase_map(
        *this, UCASEMAP_BREAK_ITERATOR_NULL
        dest, destCapacity,
        src, srcLength,
        ustrcase_internalFold, edits, errorCode);
}

U_NAMESPACE_END

/* case-insensitive string comparisons -------------------------------------- */

/*
 * This function is a copy of unorm_cmpEquivFold() minus the parts for
 * canonical equivalence.
 * Keep the functions in sync, and see there for how this works.
 * The duplication is for modularization:
 * It makes caseless (but not canonical caseless) matches independent of
 * the normalization code.
 */

/* stack element for previous-level source/decomposition pointers */
struct CmpEquivLevel {
    const UChar *start, *s, *limit;
};
typedef struct CmpEquivLevel CmpEquivLevel;

/**
 * Internal implementation code comparing string with case fold.
 * This function is called from u_strcmpFold() and u_caseInsensitivePrefixMatch().
 *
 * @param s1            input string 1
 * @param length1       length of string 1, or -1 (NULL terminated)
 * @param s2            input string 2
 * @param length2       length of string 2, or -1 (NULL terminated)
 * @param options       compare options
 * @param matchLen1     (output) length of partial prefix match in s1
 * @param matchLen2     (output) length of partial prefix match in s2
 * @param pErrorCode    receives error status
 * @return The result of comparison
 */
static int32_t _cmpFold(
            const UChar *s1, int32_t length1,
            const UChar *s2, int32_t length2,
            uint32_t options,
            int32_t *matchLen1, int32_t *matchLen2,
            UErrorCode *pErrorCode) {
    int32_t cmpRes = 0;

    const UCaseProps *csp;

    /* current-level start/limit - s1/s2 as current */
    const UChar *start1, *start2, *limit1, *limit2;

    /* points to the original start address */
    const UChar *org1, *org2;

    /* points to the end of match + 1 */
    const UChar *m1, *m2;

    /* case folding variables */
    const UChar *p;
    int32_t length;

    /* stacks of previous-level start/current/limit */
    CmpEquivLevel stack1[2], stack2[2];

    /* case folding buffers, only use current-level start/limit */
    UChar fold1[UCASE_MAX_STRING_LENGTH+1], fold2[UCASE_MAX_STRING_LENGTH+1];

    /* track which is the current level per string */
    int32_t level1, level2;

    /* current code units, and code points for lookups */
    UChar32 c1, c2, cp1, cp2;

    /* no argument error checking because this itself is not an API */

    /*
     * assume that at least the option U_COMPARE_IGNORE_CASE is set
     * otherwise this function would have to behave exactly as uprv_strCompare()
     */
    csp=ucase_getSingleton();
    if(U_FAILURE(*pErrorCode)) {
        return 0;
    }

    /* initialize */
    if(matchLen1) {
        U_ASSERT(matchLen2 !=NULL);
        *matchLen1=0;
        *matchLen2=0;
    }

    start1=m1=org1=s1;
    if(length1==-1) {
        limit1=NULL;
    } else {
        limit1=s1+length1;
    }

    start2=m2=org2=s2;
    if(length2==-1) {
        limit2=NULL;
    } else {
        limit2=s2+length2;
    }

    level1=level2=0;
    c1=c2=-1;

    /* comparison loop */
    for(;;) {
        /*
         * here a code unit value of -1 means "get another code unit"
         * below it will mean "this source is finished"
         */

        if(c1<0) {
            /* get next code unit from string 1, post-increment */
            for(;;) {
                if(s1==limit1 || ((c1=*s1)==0 && (limit1==NULL || (options&_STRNCMP_STYLE)))) {
                    if(level1==0) {
                        c1=-1;
                        break;
                    }
                } else {
                    ++s1;
                    break;
                }

                /* reached end of level buffer, pop one level */
                do {
                    --level1;
                    start1=stack1[level1].start;    /*Not uninitialized*/
                } while(start1==NULL);
                s1=stack1[level1].s;                /*Not uninitialized*/
                limit1=stack1[level1].limit;        /*Not uninitialized*/
            }
        }

        if(c2<0) {
            /* get next code unit from string 2, post-increment */
            for(;;) {
                if(s2==limit2 || ((c2=*s2)==0 && (limit2==NULL || (options&_STRNCMP_STYLE)))) {
                    if(level2==0) {
                        c2=-1;
                        break;
                    }
                } else {
                    ++s2;
                    break;
                }

                /* reached end of level buffer, pop one level */
                do {
                    --level2;
                    start2=stack2[level2].start;    /*Not uninitialized*/
                } while(start2==NULL);
                s2=stack2[level2].s;                /*Not uninitialized*/
                limit2=stack2[level2].limit;        /*Not uninitialized*/
            }
        }

        /*
         * compare c1 and c2
         * either variable c1, c2 is -1 only if the corresponding string is finished
         */
        if(c1==c2) {
            const UChar *next1, *next2;

            if(c1<0) {
                cmpRes=0;   /* c1==c2==-1 indicating end of strings */
                break;
            }

            /*
             * Note: Move the match positions in both strings at the same time
             *      only when corresponding code point(s) in the original strings
             *      are fully consumed. For example, when comparing s1="Fust" and
             *      s2="Fu\u00dfball", s2[2] is folded into "ss", and s1[2] matches
             *      the first code point in the case-folded data. But the second "s"
             *      has no matching code point in s1, so this implementation returns
             *      2 as the prefix match length ("Fu").
             */
            next1=next2=NULL;
            if(level1==0) {
                next1=s1;
            } else if(s1==limit1) {
                /* Note: This implementation only use a single level of stack.
                 *      If this code needs to be changed to use multiple levels
                 *      of stacks, the code above should check if the current
                 *      code is at the end of all stacks.
                 */
                U_ASSERT(level1==1);

                /* is s1 at the end of the current stack? */
                next1=stack1[0].s;
            }

            if (next1!=NULL) {
                if(level2==0) {
                    next2=s2;
                } else if(s2==limit2) {
                    U_ASSERT(level2==1);

                    /* is s2 at the end of the current stack? */
                    next2=stack2[0].s;
                }
                if(next2!=NULL) {
                    m1=next1;
                    m2=next2;
                }
            }
            c1=c2=-1;       /* make us fetch new code units */
            continue;
        } else if(c1<0) {
            cmpRes=-1;      /* string 1 ends before string 2 */
            break;
        } else if(c2<0) {
            cmpRes=1;       /* string 2 ends before string 1 */
            break;
        }
        /* c1!=c2 && c1>=0 && c2>=0 */

        /* get complete code points for c1, c2 for lookups if either is a surrogate */
        cp1=c1;
        if(U_IS_SURROGATE(c1)) {
            UChar c;

            if(U_IS_SURROGATE_LEAD(c1)) {
                if(s1!=limit1 && U16_IS_TRAIL(c=*s1)) {
                    /* advance ++s1; only below if cp1 decomposes/case-folds */
                    cp1=U16_GET_SUPPLEMENTARY(c1, c);
                }
            } else /* isTrail(c1) */ {
                if(start1<=(s1-2) && U16_IS_LEAD(c=*(s1-2))) {
                    cp1=U16_GET_SUPPLEMENTARY(c, c1);
                }
            }
        }

        cp2=c2;
        if(U_IS_SURROGATE(c2)) {
            UChar c;

            if(U_IS_SURROGATE_LEAD(c2)) {
                if(s2!=limit2 && U16_IS_TRAIL(c=*s2)) {
                    /* advance ++s2; only below if cp2 decomposes/case-folds */
                    cp2=U16_GET_SUPPLEMENTARY(c2, c);
                }
            } else /* isTrail(c2) */ {
                if(start2<=(s2-2) && U16_IS_LEAD(c=*(s2-2))) {
                    cp2=U16_GET_SUPPLEMENTARY(c, c2);
                }
            }
        }

        /*
         * go down one level for each string
         * continue with the main loop as soon as there is a real change
         */

        if( level1==0 &&
            (length=ucase_toFullFolding(csp, (UChar32)cp1, &p, options))>=0
        ) {
            /* cp1 case-folds to the code point "length" or to p[length] */
            if(U_IS_SURROGATE(c1)) {
                if(U_IS_SURROGATE_LEAD(c1)) {
                    /* advance beyond source surrogate pair if it case-folds */
                    ++s1;
                } else /* isTrail(c1) */ {
                    /*
                     * we got a supplementary code point when hitting its trail surrogate,
                     * therefore the lead surrogate must have been the same as in the other string;
                     * compare this decomposition with the lead surrogate in the other string
                     * remember that this simulates bulk text replacement:
                     * the decomposition would replace the entire code point
                     */
                    --s2;
                    --m2;
                    c2=*(s2-1);
                }
            }

            /* push current level pointers */
            stack1[0].start=start1;
            stack1[0].s=s1;
            stack1[0].limit=limit1;
            ++level1;

            /* copy the folding result to fold1[] */
            if(length<=UCASE_MAX_STRING_LENGTH) {
                u_memcpy(fold1, p, length);
            } else {
                int32_t i=0;
                U16_APPEND_UNSAFE(fold1, i, length);
                length=i;
            }

            /* set next level pointers to case folding */
            start1=s1=fold1;
            limit1=fold1+length;

            /* get ready to read from decomposition, continue with loop */
            c1=-1;
            continue;
        }

        if( level2==0 &&
            (length=ucase_toFullFolding(csp, (UChar32)cp2, &p, options))>=0
        ) {
            /* cp2 case-folds to the code point "length" or to p[length] */
            if(U_IS_SURROGATE(c2)) {
                if(U_IS_SURROGATE_LEAD(c2)) {
                    /* advance beyond source surrogate pair if it case-folds */
                    ++s2;
                } else /* isTrail(c2) */ {
                    /*
                     * we got a supplementary code point when hitting its trail surrogate,
                     * therefore the lead surrogate must have been the same as in the other string;
                     * compare this decomposition with the lead surrogate in the other string
                     * remember that this simulates bulk text replacement:
                     * the decomposition would replace the entire code point
                     */
                    --s1;
                    --m2;
                    c1=*(s1-1);
                }
            }

            /* push current level pointers */
            stack2[0].start=start2;
            stack2[0].s=s2;
            stack2[0].limit=limit2;
            ++level2;

            /* copy the folding result to fold2[] */
            if(length<=UCASE_MAX_STRING_LENGTH) {
                u_memcpy(fold2, p, length);
            } else {
                int32_t i=0;
                U16_APPEND_UNSAFE(fold2, i, length);
                length=i;
            }

            /* set next level pointers to case folding */
            start2=s2=fold2;
            limit2=fold2+length;

            /* get ready to read from decomposition, continue with loop */
            c2=-1;
            continue;
        }

        /*
         * no decomposition/case folding, max level for both sides:
         * return difference result
         *
         * code point order comparison must not just return cp1-cp2
         * because when single surrogates are present then the surrogate pairs
         * that formed cp1 and cp2 may be from different string indexes
         *
         * example: { d800 d800 dc01 } vs. { d800 dc00 }, compare at second code units
         * c1=d800 cp1=10001 c2=dc00 cp2=10000
         * cp1-cp2>0 but c1-c2<0 and in fact in UTF-32 it is { d800 10001 } < { 10000 }
         *
         * therefore, use same fix-up as in ustring.c/uprv_strCompare()
         * except: uprv_strCompare() fetches c=*s while this functions fetches c=*s++
         * so we have slightly different pointer/start/limit comparisons here
         */

        if(c1>=0xd800 && c2>=0xd800 && (options&U_COMPARE_CODE_POINT_ORDER)) {
            /* subtract 0x2800 from BMP code points to make them smaller than supplementary ones */
            if(
                (c1<=0xdbff && s1!=limit1 && U16_IS_TRAIL(*s1)) ||
                (U16_IS_TRAIL(c1) && start1!=(s1-1) && U16_IS_LEAD(*(s1-2)))
            ) {
                /* part of a surrogate pair, leave >=d800 */
            } else {
                /* BMP code point - may be surrogate code point - make <d800 */
                c1-=0x2800;
            }

            if(
                (c2<=0xdbff && s2!=limit2 && U16_IS_TRAIL(*s2)) ||
                (U16_IS_TRAIL(c2) && start2!=(s2-1) && U16_IS_LEAD(*(s2-2)))
            ) {
                /* part of a surrogate pair, leave >=d800 */
            } else {
                /* BMP code point - may be surrogate code point - make <d800 */
                c2-=0x2800;
            }
        }

        cmpRes=c1-c2;
        break;
    }

    if(matchLen1) {
        *matchLen1=m1-org1;
        *matchLen2=m2-org2;
    }
    return cmpRes;
}

/* internal function */
U_CFUNC int32_t
u_strcmpFold(const UChar *s1, int32_t length1,
             const UChar *s2, int32_t length2,
             uint32_t options,
             UErrorCode *pErrorCode) {
    return _cmpFold(s1, length1, s2, length2, options, NULL, NULL, pErrorCode);
}

/* public API functions */

U_CAPI int32_t U_EXPORT2
u_strCaseCompare(const UChar *s1, int32_t length1,
                 const UChar *s2, int32_t length2,
                 uint32_t options,
                 UErrorCode *pErrorCode) {
    /* argument checking */
    if(pErrorCode==0 || U_FAILURE(*pErrorCode)) {
        return 0;
    }
    if(s1==NULL || length1<-1 || s2==NULL || length2<-1) {
        *pErrorCode=U_ILLEGAL_ARGUMENT_ERROR;
        return 0;
    }
    return u_strcmpFold(s1, length1, s2, length2,
                        options|U_COMPARE_IGNORE_CASE,
                        pErrorCode);
}

U_CAPI int32_t U_EXPORT2
u_strcasecmp(const UChar *s1, const UChar *s2, uint32_t options) {
    UErrorCode errorCode=U_ZERO_ERROR;
    return u_strcmpFold(s1, -1, s2, -1,
                        options|U_COMPARE_IGNORE_CASE,
                        &errorCode);
}

U_CAPI int32_t U_EXPORT2
u_memcasecmp(const UChar *s1, const UChar *s2, int32_t length, uint32_t options) {
    UErrorCode errorCode=U_ZERO_ERROR;
    return u_strcmpFold(s1, length, s2, length,
                        options|U_COMPARE_IGNORE_CASE,
                        &errorCode);
}

U_CAPI int32_t U_EXPORT2
u_strncasecmp(const UChar *s1, const UChar *s2, int32_t n, uint32_t options) {
    UErrorCode errorCode=U_ZERO_ERROR;
    return u_strcmpFold(s1, n, s2, n,
                        options|(U_COMPARE_IGNORE_CASE|_STRNCMP_STYLE),
                        &errorCode);
}

/* internal API - detect length of shared prefix */
U_CAPI void
u_caseInsensitivePrefixMatch(const UChar *s1, int32_t length1,
                             const UChar *s2, int32_t length2,
                             uint32_t options,
                             int32_t *matchLen1, int32_t *matchLen2,
                             UErrorCode *pErrorCode) {
    _cmpFold(s1, length1, s2, length2, options,
        matchLen1, matchLen2, pErrorCode);
}
