/*
 * Implementation of Part 5 of the DICOM standard: Data Structures and Encoding.
 */

#include "config.h"

#ifdef _WIN32
// the Windows CRT considers strdup and strcpy unsafe
#define _CRT_SECURE_NO_WARNINGS
// and deprecates strdup
#define strdup(v) _strdup(v)
#endif

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "utarray.h"
#include "uthash.h"

#include <dicom/dicom.h>
#include "pdicom.h"


struct _DcmElement {
    uint32_t tag;
    DcmVR vr;
    uint32_t length;
    uint32_t vm;
    bool assigned;

    // Store values for multiplicity 1 (the most common case) 
    // inside the element to reduce malloc/frees during build
    union {
        union {
            float fl;
            double fd;
            int16_t ss;
            int32_t sl;
            int64_t sv;
            uint16_t us;
            uint32_t ul;
            uint64_t uv;

            char *str;

            // Binary value (multiplicity 1)
            char *bytes;

            // Sequence value (multiplicity 1)
            DcmSequence *sq;

        } single;

        union {
            // Numeric value (multiplicity 2-n)
            float *fl;
            double *fd;
            int16_t *ss;
            int32_t *sl;
            int64_t *sv;
            uint16_t *us;
            uint32_t *ul;
            uint64_t *uv;

            // Character string value (multiplicity 2-n)
            char **str;

        } multi;
    } value;

    // Free these on destroy
    void *value_pointer;
    char **value_pointer_array;
    DcmSequence *sequence_pointer;

    UT_hash_handle hh;
};


struct _DcmSequence {
    UT_array *items;
    bool is_locked;
};


struct _DcmDataSet {
    DcmElement *elements;
    bool is_locked;
};


struct _DcmFrame {
    uint32_t number;
    const char *data;
    uint32_t length;
    uint16_t rows;
    uint16_t columns;
    uint16_t samples_per_pixel;
    uint16_t bits_allocated;
    uint16_t bits_stored;
    uint16_t high_bit;
    uint16_t pixel_representation;
    uint16_t planar_configuration;
    const char *photometric_interpretation;
    const char *transfer_syntax_uid;
};


struct _DcmBOT {
    uint32_t num_frames;
    ssize_t *offsets;
    ssize_t first_frame_offset;
};


struct SequenceItem {
    DcmDataSet *dataset;
};


static struct SequenceItem *create_sequence_item(DcmError **error,
                                                 DcmDataSet *dataset)
{
    struct SequenceItem *item = DCM_NEW(error, struct SequenceItem);
    if (item == NULL) {
        return NULL;
    }
    item->dataset = dataset;
    dcm_dataset_lock(item->dataset);
    return item;
}


static void copy_sequence_item_icd(void *_dst_item, const void *_src_item)
{
    struct SequenceItem *dst_item = (struct SequenceItem *) _dst_item;
    struct SequenceItem *src_item = (struct SequenceItem *) _src_item;
    dst_item->dataset = src_item->dataset;
    dcm_dataset_lock(dst_item->dataset);
}


static void destroy_sequence_item_icd(void *_item)
{
    if (_item) {
        struct SequenceItem *item = (struct SequenceItem *) _item;
        if (item) {
            if (item->dataset) {
                dcm_dataset_destroy(item->dataset);
                item->dataset = NULL;
            }
            // utarray frees the memory of the item itself
        }
    }
}


static UT_icd sequence_item_icd = {
    sizeof(struct SequenceItem),
    NULL,
    copy_sequence_item_icd,
    destroy_sequence_item_icd
};


static int compare_tags(const void *a, const void *b)
{
   return ( *(uint32_t*)a - *(uint32_t*)b );
}


static void element_set_length(DcmElement *element, uint32_t length)
{
    uint32_t even_length = length % 2 != 0 ? length + 1 : length;

    if (element->length == 0) {
        element->length = even_length;
    } 

    // length is set in two ways: 
    //
    // - from the element header read in file parse
    //
    // - when we later set a value for the element, we compute a length from
    //   the value
    //
    // the computed length won't always match the length in the header, since 
    // the length of compound elements will change with the coding we use 
    // (eg. implicit vs explicit), so we only record the length if it hasn't
    // been set before, and we can't throw errors for mismatches
}


DcmElement *dcm_element_create(DcmError **error, uint32_t tag, uint32_t length)
{
    DcmElement *element = DCM_NEW(error, DcmElement);
    if (element == NULL) {
        return NULL;
    }
    element->tag = tag;
    element->vr = dcm_dict_lookup_vr(tag);
    element_set_length(element, length);
    if (element->vr == DCM_VR_uk) {
        dcm_element_destroy(element);
        return NULL;
    }

    return element;
}


void dcm_element_destroy(DcmElement *element)
{
    if (element) {
        dcm_log_debug("Destroy Data Element '%08X'.", element->tag);
        if(element->sequence_pointer) {
            dcm_sequence_destroy(element->sequence_pointer);
        }
        if(element->value_pointer) {
            free(element->value_pointer);
        }
        if(element->value_pointer_array) {
            dcm_free_string_array(element->value_pointer_array, element->vm);
        }
        free(element);
    }
}


uint16_t dcm_element_get_group_number(const DcmElement *element)
{
    return (uint16_t)(element->tag >> 16);
}


uint16_t dcm_element_get_element_number(const DcmElement *element)
{
    return (uint16_t)(element->tag);
}


uint32_t dcm_element_get_tag(const DcmElement *element)
{
    return element->tag;
}


DcmVR dcm_element_get_vr(const DcmElement *element)
{
    return element->vr;
}


uint32_t dcm_element_get_vm(const DcmElement *element)
{
    return element->vm;
}


bool dcm_element_is_multivalued(const DcmElement *element)
{
    return element->vm > 1;
}


uint32_t dcm_element_get_length(const DcmElement *element)
{
    return element->length;
}


// check, set, get string value representations

static bool element_check_index(DcmError **error, 
                                const DcmElement *element, uint32_t index)
{
    if (index >= element->vm) {
        dcm_error_set(error, DCM_ERROR_CODE_INVALID,
                      "Data Element index out of range",
                      "Element tag %08X has VM of %d, index %d is out of range",
                      element->tag,
                      element->vm,
                      index);
        return false;
    }

    return true;
}


static bool element_check_string(DcmError **error,
                                 const DcmElement *element)
{
    DcmVRClass klass = dcm_dict_vr_class(element->vr);
    if (klass != DCM_CLASS_STRING_MULTI && klass != DCM_CLASS_STRING_SINGLE) {
        dcm_error_set(error, DCM_ERROR_CODE_INVALID,
                      "Data Element is not string",
                      "Element tag %08X has VR %s with no string value",
                      element->tag,
                      dcm_dict_vr_to_str(element->vr));
        return false;
    }

    return true;
}


static bool element_check_assigned(DcmError **error,
                                   const DcmElement *element)
{
    if (!element->assigned) {
        dcm_error_set(error, DCM_ERROR_CODE_INVALID,
                      "Data Element not assigned a value",
                      "Element tag %08X has not been assigned a value",
                      element->tag);
        return false;
    }

    return true;
}


static bool element_check_not_assigned(DcmError **error,
                                       const DcmElement *element)
{
    if (element->assigned) {
        dcm_error_set(error, DCM_ERROR_CODE_INVALID,
                      "Data Element assigned twice",
                      "Element tag %08X has been previously assigned a value",
                      element->tag);
        return false;
    }

    return true;
}


bool dcm_element_get_value_string(DcmError **error,
                                  const DcmElement *element, 
                                  uint32_t index,
                                  const char **value)
{
    if (!element_check_assigned(error, element) ||
        !element_check_string(error, element) ||
        !element_check_index(error, element, index)) {
        return false;
    }

    if (element->vm == 1) {
        *value = element->value.single.str;
    } else {
        *value = element->value.multi.str[index];
    }

    return true;
}


static bool element_check_capacity(DcmError **error, 
                                   DcmElement *element, uint32_t capacity)
{
    uint32_t i;

    bool was_assigned = element->assigned;

    // we have to turn on "assigned" for this func so we can read out values
    element->assigned = true;

    for (i = 0; i < element->vm; i++) {
        const char *value;
        if (!dcm_element_get_value_string(error, element, i, &value)) {
            element->assigned = was_assigned;
            return false;
        }

        size_t length = strlen(value);
        if (length > capacity) {
            element->assigned = was_assigned;
            dcm_error_set(error, DCM_ERROR_CODE_INVALID,
                          "Data Element capacity check failed",
                          "Value of Data Element '%08X' exceeds "
                          "maximum length of Value Representation",
                          "(%d)",
                          element->tag,
                          capacity);
            return false;
        }
    }

    element->assigned = was_assigned;

    return true;
}


static bool dcm_element_validate(DcmError **error, DcmElement *element)
{
    DcmVR correct_vr = dcm_dict_lookup_vr(element->tag);
    DcmVRClass klass = dcm_dict_vr_class(element->vr);

    if (!element_check_not_assigned(error, element)) {
        return false;
    }

    if (element->vr != correct_vr || klass == DCM_CLASS_ERROR) {
        dcm_error_set(error, DCM_ERROR_CODE_INVALID,
                      "Data Element validation failed",
                      "Bad VR for tag %08X, should be %s",
                      element->tag,
                      dcm_dict_vr_to_str(element->vr));
        return false;
    }

    if (klass == DCM_CLASS_NUMERIC) {
        if (element->length != element->vm * dcm_dict_vr_size(element->vr)) {
            dcm_error_set(error, DCM_ERROR_CODE_INVALID,
                          "Data Element validation failed",
                          "Bad length for numeric tag %08X",
                          element->tag);
            return false;
        }
    }

    if (klass == DCM_CLASS_STRING_MULTI || klass == DCM_CLASS_STRING_SINGLE) {
        uint32_t capacity = dcm_dict_vr_capacity(element->vr);
        if (!element_check_capacity(error, element, capacity)) {
            return false;
        }
    }

    element->assigned = true;

    return true;
}


bool dcm_element_set_value_string_multi(DcmError **error,
                                        DcmElement *element,
                                        char **values,
                                        uint32_t vm,
                                        bool steal)
{
    if (!element_check_not_assigned(error, element) ||
        !element_check_string(error, element)) {
        return false;
    }

    if (vm == 1) {
        element->value.single.str = values[0];
        element->vm = 1;
    } else {
        DcmVRClass klass = dcm_dict_vr_class(element->vr);
        if (klass != DCM_CLASS_STRING_MULTI) {
            dcm_error_set(error, DCM_ERROR_CODE_INVALID,
                          "Data Element is not multi-valued string",
                          "Element tag %08X has VR %s with only a string value",
                          element->tag,
                          dcm_dict_vr_to_str(element->vr));
            return false;
        }

        element->value.multi.str = values;
        element->vm = vm;
    }

    size_t length = 0;
    for (uint32_t i = 0; i < vm; i++) {
        length += strlen(values[i]);
    }
    if (vm > 1) {
        // add the separator characters
        length += vm - 1;
    }
    element_set_length(element, length);

    if (!dcm_element_validate(error, element)) {
        return false;
    }

    if (steal) {
        element->value_pointer_array = values;
    }

    return true;
}


static bool element_set_value_string(DcmError **error, 
                                     DcmElement *element, 
                                     char *value,
                                     bool steal)
{
    if (!element_check_not_assigned(error, element) ||
        !element_check_string(error, element)) {
        return false;
    }

    DcmVRClass klass = dcm_dict_vr_class(element->vr);
    if (klass == DCM_CLASS_STRING_MULTI) {
        uint32_t vm;
        char **values = dcm_parse_character_string(error, value, &vm);
        if (values == NULL) {
            return false;
        }

        if (!dcm_element_set_value_string_multi(error, 
                                                element, values, vm, true)) {
            dcm_free_string_array(values, vm);
            return false;
        }
    } else {
        element->value.single.str = value;
        element->vm = 1;
        element_set_length(element, strlen(value));

        if (!dcm_element_validate(error, element)) {
            return false;
        }
    }

    if (steal) {
        element->value_pointer = value;
    }

    return true;
}


bool dcm_element_set_value_string(DcmError **error, 
                                  DcmElement *element, 
                                  char *value)
{
    return element_set_value_string(error, element, value, true);
}


bool dcm_element_set_value_string_static(DcmError **error, 
                                         DcmElement *element, 
                                         const char *value)
{
    return element_set_value_string(error, element, (char *) value, false);
}



// integer numeric types

// use a VR to marshall an int pointer into a int64_t
static int64_t value_to_int64(DcmVR vr, int *value)
{
    uint64_t result;

#define PEEK(TYPE) result = *((TYPE *) value)
    DCM_SWITCH_NUMERIC(vr, PEEK);
#undef PEEK

    return result;
}


// use a VR to write an int64_t to an int pointer
static void int64_to_value(DcmVR vr, int *result, int64_t value)
{
#define POKE(TYPE) *((TYPE *) result) = value;
    DCM_SWITCH_NUMERIC(vr, POKE);
#undef POKE
}


// use a VR to copy any numeric value (not just int as above)
static void value_to_value(DcmVR vr, int *result, int *value)
{
#define COPY(TYPE) *((TYPE *)result) = *((TYPE *)value);
    DCM_SWITCH_NUMERIC(vr, COPY);
#undef COPY
}


static bool element_check_numeric(DcmError **error,
                                  const DcmElement *element)
{
    DcmVRClass klass = dcm_dict_vr_class(element->vr);
    if (klass != DCM_CLASS_NUMERIC) {
      dcm_error_set(error, DCM_ERROR_CODE_INVALID,
                    "Data Element is not numeric",
                    "Element tag %08X is not numeric",
                    element->tag);
      return false;
    }

    return true;
}


static bool element_check_integer(DcmError **error,
                                  const DcmElement *element)
{
    if (element->vr == DCM_VR_FL || element->vr == DCM_VR_FD) {
      dcm_error_set(error, DCM_ERROR_CODE_INVALID,
                    "Data Element is not integer",
                    "Element tag %08X is not integer",
                    element->tag);
      return false;
    }

    return true;
}


bool dcm_element_get_value_integer(DcmError **error,
                                   const DcmElement *element, 
                                   uint32_t index,
                                   int64_t *value)
{
    if (!element_check_assigned(error, element) || 
        !element_check_numeric(error, element) || 
        !element_check_integer(error, element) ||
        !element_check_index(error, element, index)) {
        return false;
    }

    int *element_value;
    if (element->vm == 1) {
        element_value = (int *) &element->value.single.sl;
    } else {
        size_t size = dcm_dict_vr_size(element->vr);
        unsigned char *base = (unsigned char *) element->value.multi.sl;
        element_value = (int *)(base + size * index);
    }
    *value = value_to_int64(element->vr, element_value);

    return true;
}


bool dcm_element_set_value_integer(DcmError **error, 
                                   DcmElement *element, 
                                   int64_t value)
{
    if (!element_check_not_assigned(error, element) || 
        !element_check_numeric(error, element) || 
        !element_check_integer(error, element)) {
        return false;
    }

    int *element_value = (int *) &element->value.single.sl;
    int64_to_value(element->vr, element_value, value);
    element->vm = 1;
    element_set_length(element, dcm_dict_vr_size(element->vr));

    if (!dcm_element_validate(error, element)) {
        return false;
    }

    return true;
}


bool dcm_element_set_value_numeric_multi(DcmError **error, 
                                         DcmElement *element, 
                                         int *value,
                                         uint32_t vm,
                                         bool steal)
{
    if (!element_check_not_assigned(error, element) || 
        !element_check_numeric(error, element)) {
        return false;
    }

    if (vm == 1) {
        // actually does all numeric types
        value_to_value(element->vr, (int *)&element->value.single.sl, value);
    } else {
        // this will work for all numeric types, since we're just setting a
        // pointer
        element->value.multi.sl = (int32_t *)value;
    }

    element->vm = vm;
    element_set_length(element, vm * dcm_dict_vr_size(element->vr));

    if (!dcm_element_validate(error, element)) {
        return false;
    }

    if (steal) {
        element->value_pointer = value;
    }

    return true;
}


// the float values

// use a VR to marshall a double pointer into a float
static double value_to_double(DcmVR vr, double *value)
{
    double result;

#define PEEK(TYPE) result = *((TYPE *) value)
    DCM_SWITCH_NUMERIC(vr, PEEK);
#undef PEEK

    return result;
}


// use a VR to write a double to a double pointer
static void double_to_value(DcmVR vr, double *result, double value)
{
#define POKE(TYPE) *((TYPE *) result) = value;
    DCM_SWITCH_NUMERIC(vr, POKE);
#undef POKE
}


static bool element_check_float(DcmError **error,
                                const DcmElement *element)
{
    if (element->vr != DCM_VR_FL && element->vr != DCM_VR_FD) {
      dcm_error_set(error, DCM_ERROR_CODE_INVALID,
                    "Data Element is not float",
                    "Element tag %08X is not one of the float types",
                    element->tag);
      return false;
    }

    return true;
}


bool dcm_element_get_value_double(DcmError **error,
                                  const DcmElement *element,   
                                  uint32_t index,
                                  double *value)
{
    if (!element_check_assigned(error, element) || 
        !element_check_numeric(error, element) || 
        !element_check_float(error, element) ||
        !element_check_index(error, element, index)) {
        return false;
    }

    double *element_value;
    if (element->vm == 1) {
        element_value = (double *) &element->value.single.fd;
    } else {
        size_t size = dcm_dict_vr_size(element->vr);
        unsigned char *base = (unsigned char *) element->value.multi.fd;
        element_value = (double *)(base + size * index);
    }
    *value = value_to_double(element->vr, element_value);

    return true;
}


bool dcm_element_set_value_double(DcmError **error, 
                                  DcmElement *element, 
                                  double value)
{
    if (!element_check_not_assigned(error, element) || 
        !element_check_numeric(error, element) || 
        !element_check_float(error, element)) {
        return false;
    }

    double *element_value = (double *) &element->value.single.fd;
    double_to_value(element->vr, element_value, value);
    element->vm = 1;
    element_set_length(element, dcm_dict_vr_size(element->vr));

    if (!dcm_element_validate(error, element)) {
        return false;
    }

    return true;
}


// The VRs with binary values

static bool element_check_binary(DcmError **error,
                                 const DcmElement *element)
{
    DcmVRClass klass = dcm_dict_vr_class(element->vr);
    if (klass != DCM_CLASS_BINARY) {
      dcm_error_set(error, DCM_ERROR_CODE_INVALID,
                    "Data Element is not binary",
                    "Element tag %08X does not have a binary value",
                    element->tag);
      return false;
    }

    return true;
}


bool dcm_element_get_value_binary(DcmError **error,
                                  const DcmElement *element,   
                                  const char **value)
{
    if (!element_check_assigned(error, element) || 
        !element_check_binary(error, element)) {
        return false;
    }

    *value = element->value.single.bytes;

    return true;
}


bool dcm_element_set_value_binary(DcmError **error, 
                                  DcmElement *element, 
                                  char *value,
                                  uint32_t length,
                                  bool steal)
{
    if (!element_check_not_assigned(error, element) || 
        !element_check_binary(error, element)) {
        return false;
    }

    element->vm = 1;
    element->value.single.bytes = value;
    element_set_length(element, length);

    if (!dcm_element_validate(error, element)) {
        return false;
    }

    if (steal) {
        element->value_pointer = value;
    }

    return true;
}


// Sequence Data Element

static bool element_check_sequence(DcmError **error,
                                   const DcmElement *element)
{
    DcmVRClass klass = dcm_dict_vr_class(element->vr);
    if (klass != DCM_CLASS_SEQUENCE) {
      dcm_error_set(error, DCM_ERROR_CODE_INVALID,
                    "Data Element is not seeuence",
                    "Element tag %08X does not have a seeuence value",
                    element->tag);
      return false;
    }

    return true;
}


bool dcm_element_get_value_sequence(DcmError **error,
                                    const DcmElement *element,   
                                    DcmSequence **value)
{
    if (!element_check_assigned(error, element) || 
        !element_check_sequence(error, element)) {
        return false;
    }

    dcm_sequence_lock(element->value.single.sq);
    *value = element->value.single.sq;

    return true;
}


bool dcm_element_set_value_sequence(DcmError **error, 
                                    DcmElement *element,   
                                    DcmSequence *value)
{
    if (!element_check_not_assigned(error, element) || 
        !element_check_sequence(error, element)) {
        return false;
    }

    uint32_t seq_count = dcm_sequence_count(value);
    uint32_t length = 0;
    for (uint32_t i = 0; i < seq_count; i++) {
        DcmDataSet *item = dcm_sequence_get(error, value, i);
        if (item == NULL) {
            return false;
        }
        for (DcmElement *element = item->elements;
            element; 
            element = element->hh.next) {
            length += element->length;
        }
    }
    element_set_length(element, length);

    element->value.single.sq = value;
    element->vm = 1;

    if (!dcm_element_validate(error, element)) {
        return false;
    }

    element->sequence_pointer = value;

    return true;
}


DcmElement *dcm_element_clone(DcmError **error, const DcmElement *element)
{
    uint32_t i;

    dcm_log_debug("Clone Data Element '%08X'.", element->tag);
    DcmElement *clone = dcm_element_create(error, 
                                           element->tag, element->length);
    if (clone == NULL) {
        return NULL;
    }

    DcmVRClass klass = dcm_dict_vr_class(element->vr);
    switch (klass) {
        case DCM_CLASS_SEQUENCE:
            DcmSequence *from_seq;
            if (!dcm_element_get_value_sequence(error, element, &from_seq)) {
                dcm_element_destroy(clone);
                return NULL;
            }

            // Copy each data set in sequence
            uint32_t count = dcm_sequence_count(from_seq);
            DcmSequence *seq = dcm_sequence_create(error);

            for (i = 0; i < count; i++) {
                DcmDataSet *item = dcm_sequence_get(error, from_seq, i);
                if (item == NULL) {
                    dcm_sequence_destroy(seq);
                    dcm_element_destroy(clone);
                    return NULL;
                }

                DcmDataSet *cloned_item = dcm_dataset_clone(error, item);
                if (cloned_item == NULL) {
                    dcm_sequence_destroy(seq);
                    dcm_element_destroy(clone);
                    return NULL;
                }

                dcm_sequence_append(error, seq, cloned_item);
            }

            if (!dcm_element_set_value_sequence(error, clone, seq)) {
                dcm_sequence_destroy(seq);
                dcm_element_destroy(clone);
                return NULL;
            }

            break;

        case DCM_CLASS_STRING_MULTI:
        case DCM_CLASS_STRING_SINGLE:
            // all the string types
            if (element->vm == 1 && element->value.single.str) {
                clone->value.single.str = dcm_strdup(error, 
                                                     element->value.single.str);
                if (clone->value.single.str == NULL) {
                    dcm_element_destroy(clone);
                    return NULL;
                }
                clone->value_pointer = clone->value.single.str;
                clone->vm = 1;
            } else if (element->vm > 1 && element->value.multi.str) {
                clone->value.multi.str = DCM_NEW_ARRAY(error, 
                                                       element->vm, char *);
                if (clone->value.multi.str == NULL) {
                    dcm_element_destroy(clone);
                    return NULL;
                }
                clone->value_pointer_array = clone->value.multi.str;

                for (i = 0; i < element->vm; i++) {
                    clone->value.multi.str[i] = dcm_strdup(error, 
                                                           element->
                                                           value.multi.str[i]);
                    if (clone->value.multi.str[i] == NULL) {
                        dcm_element_destroy(clone);
                        return NULL;
                    }
                }
                clone->vm = element->vm;
            }

            break;

        case DCM_CLASS_BINARY:
            if (element->value.single.bytes) {
                clone->value.single.bytes = DCM_MALLOC(error, element->length);
                if (clone->value.single.bytes == NULL) {
                    dcm_element_destroy(clone);
                    return NULL;
                }
                memcpy(clone->value.single.bytes,
                       element->value.single.bytes,
                       element->length);
                clone->value_pointer = clone->value.single.bytes;
                clone->vm = 1;
            }
            break;

        case DCM_CLASS_NUMERIC:
            if (element->vm == 1) {
                clone->value = element->value;
                clone->vm = 1;
            } else {
                // some kind of numeric value .. we use the float pointer, 
                // but this will do all the numeric array types
                size_t size = dcm_dict_vr_size(element->vr);
                clone->value.multi.fl = dcm_calloc(error, element->vm, size);
                if (clone->value.multi.fl == NULL) {
                    dcm_element_destroy(clone);
                    return NULL;
                }
                memcpy(clone->value.multi.fl,
                       element->value.multi.fl,
                       element->vm * size);
                clone->value_pointer = clone->value.multi.fl;
                clone->vm = element->vm;
            }
            break;

        default:
            break;
    }

    if (!dcm_element_validate(error, clone)) {
        dcm_element_destroy(clone);
        return NULL;
    }

    return clone;
}


// printing elements

static void element_print_integer(const DcmElement *element,
                                  uint32_t index)
{
    int64_t value;
    (void) dcm_element_get_value_integer(NULL, element, index, &value);
    if (element->vr == DCM_VR_UV) {
        printf("%lu", (uint64_t)value);
    } else {
        printf("%ld", value);
    }
}


static void element_print_float(const DcmElement *element,
                                uint32_t index)
{
    double value;
    (void) dcm_element_get_value_double(NULL, element, index, &value);
    printf("%g", value);
}


static void element_print_string(const DcmElement *element,
                                 uint32_t index)
{
    const char *value;
    (void) dcm_element_get_value_string(NULL, element, index, &value);
    printf("%s", value);
}


void dcm_element_print(const DcmElement *element, uint8_t indentation)
{
    DcmVRClass klass = dcm_dict_vr_class(element->vr);
    const uint8_t num_indent = indentation * 2;
    const uint8_t num_indent_next = (indentation + 1) * 2;

    uint32_t i;

    if (dcm_is_public_tag(element->tag)) {
        const char *keyword = dcm_dict_lookup_keyword(element->tag);
        printf("%*.*s(%04X,%04X) %s | %s",
               num_indent,
               num_indent,
               "                                   ",
               dcm_element_get_group_number(element),
               dcm_element_get_element_number(element),
               keyword,
               dcm_dict_vr_to_str(element->vr));
    } else {
        printf("%*.*s (%04X,%04X) | %s",
               num_indent,
               num_indent,
               "                                   ",
               dcm_element_get_group_number(element),
               dcm_element_get_element_number(element),
               dcm_dict_vr_to_str(element->vr));
    }

    if (element->vr == DCM_VR_SQ) {
        DcmSequence *sequence;
        (void) dcm_element_get_value_sequence(NULL, element, &sequence);
        uint32_t sequence_count = dcm_sequence_count(sequence);
        if (sequence_count == 0) {
            printf(" | [");
        } else {
            printf(" | [\n");
        }
        for (i = 0; i < sequence_count; i++) {
            printf("%*.*s---Item #%d---\n",
                   num_indent_next,
                   num_indent_next,
                   "                                   ",
                   i + 1);
            DcmDataSet *item = dcm_sequence_get(NULL, sequence, i);
            dcm_dataset_print(item, indentation + 1);
        }
        printf("%*.*s]\n",
               num_indent,
               num_indent,
               "                                   ");
    } else {
        printf(" | %u | ", element->length);

        if (element->vm > 1) {
            printf("[");
        }
        for (i = 0; i < element->vm; i++) {
            switch (klass) {
                case DCM_CLASS_NUMERIC:
                    if (element->vr == DCM_VR_FL || element->vr == DCM_VR_FD) {
                        element_print_float(element, i);
                    } else {
                        element_print_integer(element, i);
                    }
                    break;

                case DCM_CLASS_STRING_SINGLE:
                case DCM_CLASS_STRING_MULTI:
                    element_print_string(element, i);
                    break;

                case DCM_CLASS_BINARY:
                    break;

                case DCM_CLASS_SEQUENCE:
                default:
                    dcm_log_warning("Unexpected Value Representation.");
            }

            if (element->vm > 1) {
                if (i == (element->vm - 1)) {
                    printf("]");
                } else {
                    printf(", ");
                }
            }
        }
        printf("\n");
    }
}


// Datasets

DcmDataSet *dcm_dataset_create(DcmError **error)
{
    dcm_log_debug("Create Data Set.");
    DcmDataSet *dataset = DCM_NEW(error, DcmDataSet);
    if (dataset == NULL) {
        return NULL;
    }
    dataset->elements = NULL;
    dataset->is_locked = false;
    return dataset;
}


DcmDataSet *dcm_dataset_clone(DcmError **error, const DcmDataSet *dataset)
{
    dcm_log_debug("Clone Data Set.");
    DcmDataSet *cloned_dataset = dcm_dataset_create(error);
    if (cloned_dataset == NULL) {
        return NULL;
    }

    DcmElement *element;
    DcmElement *cloned_element;
    for(element = dataset->elements; element; element = element->hh.next) {
        cloned_element = dcm_element_clone(error, element);
        if (cloned_element == NULL) {
            dcm_dataset_destroy(cloned_dataset);
            return NULL;
        }
        if (!dcm_dataset_insert(error, cloned_dataset, cloned_element)) {
            dcm_dataset_destroy(cloned_dataset);
            return NULL;
        }
    }

    return cloned_dataset;
}


bool dcm_dataset_insert(DcmError **error, 
                        DcmDataSet *dataset, DcmElement *element)
{
    assert(dataset);
    assert(element);

    dcm_log_debug("Insert Data Element '%08X' into Data Set.", element->tag);
    if (dataset->is_locked) {
        dcm_error_set(error, DCM_ERROR_CODE_INVALID,
                      "Data Set is locked and cannot be modified",
                      "Inserting Data Element '%08X' into Data Set failed",
                      element->tag);
        dcm_element_destroy(element);
        return false;
    }

    DcmElement *matched_element;
    HASH_FIND_INT(dataset->elements, &element->tag, matched_element);
    if (matched_element) {
        dcm_error_set(error, DCM_ERROR_CODE_INVALID,
                      "Element already exists",
                      "Inserting Data Element '%08X' into Data Set failed",
                      element->tag);
        dcm_element_destroy(element);
        return false;
    }

    HASH_ADD_INT(dataset->elements, tag, element);

    return true;
}


bool dcm_dataset_remove(DcmError **error, DcmDataSet *dataset, uint32_t tag)
{
    assert(dataset);

    dcm_log_debug("Remove Data Element '%08X' from Data Set.", tag);
    if (dataset->is_locked) {
        dcm_error_set(error, DCM_ERROR_CODE_INVALID,
                      "Data Set is locked and cannot be modified",
                      "Removing Data Element '%08X' from Data Set failed",
                      tag);
        return false;
    }

    DcmElement *matched_element = dcm_dataset_get(error, dataset, tag);
    if (matched_element == NULL) {
        return false;
    }

    HASH_DEL(dataset->elements, matched_element);
    dcm_element_destroy(matched_element);

    return true;
}


DcmElement *dcm_dataset_get_clone(DcmError **error,
                                  const DcmDataSet *dataset, uint32_t tag)
{
    assert(dataset);
    DcmElement *element;

    dcm_log_debug("Copy Data Element '%08X' from Data Set.", tag);
    HASH_FIND_INT(dataset->elements, &tag, element);
    if (element == NULL) {
        dcm_error_set(error, DCM_ERROR_CODE_INVALID,
                      "Could not find Data Element",
                      "Getting Data Element '%08X' from Data Set failed",
                      tag);
        return NULL;
    }
    return dcm_element_clone(error, element);
}


DcmElement *dcm_dataset_get(DcmError **error, 
                            const DcmDataSet *dataset, uint32_t tag)
{
    assert(dataset);
    DcmElement *element;

    dcm_log_debug("Get Data Element '%08X' from Data Set.", tag);
    element = dcm_dataset_contains(dataset, tag);
    if (element == NULL) {
        dcm_error_set(error, DCM_ERROR_CODE_INVALID,
                      "Could not find Data Element",
                      "Getting Data Element '%08X' from Data Set failed",
                      tag);
    }

    return element;
}


void dcm_dataset_foreach(const DcmDataSet *dataset,
                         void (*fn)(const DcmElement *element))
{
    assert(dataset);
    DcmElement *element;

    for(element = dataset->elements; element; element = element->hh.next) {
        fn(element);
    }
}


DcmElement *dcm_dataset_contains(const DcmDataSet *dataset, uint32_t tag)
{
    assert(dataset);

    DcmElement *element;
    HASH_FIND_INT(dataset->elements, &tag, element);

    return element;
}


uint32_t dcm_dataset_count(const DcmDataSet *dataset)
{
    assert(dataset);

    uint32_t num_users = HASH_COUNT(dataset->elements);
    return num_users;
}


void dcm_dataset_copy_tags(const DcmDataSet *dataset, 
                           uint32_t *tags, uint32_t n)
{
    assert(dataset);
    uint32_t i;
    DcmElement *element;

    for(i = 0, element = dataset->elements; 
        element && i < n; 
        element = element->hh.next, i++) {
        tags[i] = element->tag;
    }

    qsort(tags, n, sizeof(uint32_t), compare_tags);
}


void dcm_dataset_print(const DcmDataSet *dataset, uint8_t indentation)
{
    assert(dataset);
    uint32_t i;
    DcmElement *element;

    uint32_t n = dcm_dataset_count(dataset);
    uint32_t *tags = DCM_NEW_ARRAY(NULL, n, uint32_t);
    if (tags == NULL) {
        return;
    }
    dcm_dataset_copy_tags(dataset, tags, n);

    for(i = 0; i < n; i++) {
        element = dcm_dataset_get(NULL, dataset, tags[i]);
        if (element == NULL) {
            dcm_log_warning("Missing tag.");
            free(tags);
            return;
        }
        dcm_element_print(element, indentation);
    }

    free(tags);
}


void dcm_dataset_lock(DcmDataSet *dataset)
{
    dataset->is_locked = true;
}


bool dcm_dataset_is_locked(const DcmDataSet *dataset)
{
    return dataset->is_locked;
}


void dcm_dataset_destroy(DcmDataSet *dataset)
{
    DcmElement *element, *tmp;

    if (dataset) {
        HASH_ITER(hh, dataset->elements, element, tmp) {
            HASH_DEL(dataset->elements, element);
            dcm_element_destroy(element);
        }
        free(dataset);
        dataset = NULL;
    }
}


// Sequences

DcmSequence *dcm_sequence_create(DcmError **error)
{
    DcmSequence *seq = DCM_NEW(error, DcmSequence);
    if (seq == NULL) {
        return NULL;
    }

    UT_array *items;
    utarray_new(items, &sequence_item_icd);
    if (items == NULL) {
        dcm_error_set(error, DCM_ERROR_CODE_NOMEM,
                      "Out of memory",
                      "Creation of Sequence failed");
        free(seq);
        return NULL;
    }
    seq->items = items;
    seq->is_locked = false;

    return seq;
}


bool dcm_sequence_append(DcmError **error, DcmSequence *seq, DcmDataSet *item)
{
    assert(seq);
    assert(item);

    dcm_log_debug("Append item to Sequence.");
    if (seq->is_locked) {
        dcm_error_set(error, DCM_ERROR_CODE_INVALID,
                      "Appending item to Sequence failed",
                      "Sequence is locked and cannot be modified.");
        dcm_dataset_destroy(item);
        return false;
    }

    /**
     * The SequenceItem is just a thin wrapper around a DcmDataSet object as a
     * handle for utarray. Under the hood, utarray frees the memory of the
     * DcmDataSet object when the array item gets destroyed. However, utarray
     * does not free the memory of the item handle. Therefore, we need to free
     * the memory of the item handle after the item was added to the array.
     */
    struct SequenceItem *item_handle = create_sequence_item(error, item);
    utarray_push_back(seq->items, item_handle);
    free(item_handle);

    return true;
}


DcmDataSet *dcm_sequence_get(DcmError **error, 
                             const DcmSequence *seq, uint32_t index)
{
    assert(seq);

    dcm_log_debug("Get item #%i of Sequence.", index);
    uint32_t length = utarray_len(seq->items);
    if (index >= length) {
        dcm_error_set(error, DCM_ERROR_CODE_INVALID,
                      "Getting item of Sequence failed",
                      "Index %i exceeds length of sequence %i",
                      index, length);
        return NULL;
    }

    struct SequenceItem *item_handle = utarray_eltptr(seq->items, index);
    if (item_handle == NULL) {
        dcm_error_set(error, DCM_ERROR_CODE_INVALID,
                      "Getting item of Sequence failed",
                      "Getting item #%i of Sequence failed", index);
        return NULL;
    }
    if (item_handle->dataset == NULL) {
        dcm_error_set(error, DCM_ERROR_CODE_INVALID,
                      "Getting item of Sequence failed",
                      "Getting item #%i of Sequence failed", index);
        return NULL;
    }
    dcm_dataset_lock(item_handle->dataset);

    return item_handle->dataset;
}


void dcm_sequence_foreach(const DcmSequence *seq,
                          void (*fn)(const DcmDataSet *item))
{
    assert(seq);
    uint32_t i;
    struct SequenceItem *item_handle;

    uint32_t length = utarray_len(seq->items);
    for (i = 0; i < length; i++) {
        item_handle = utarray_eltptr(seq->items, i);
        dcm_dataset_lock(item_handle->dataset);
        fn(item_handle->dataset);
    }
}


void dcm_sequence_remove(DcmError **error, DcmSequence *seq, uint32_t index)
{
    assert(seq);
    if (seq->is_locked) {
        dcm_error_set(error, DCM_ERROR_CODE_INVALID,
                      "Removing item from Sequence failed",
                      "Sequence is locked and cannot be modified.");
        return;
    }
    dcm_log_debug("Remove item #%i from Sequence.", index);
    uint32_t length = utarray_len(seq->items);
    if (index >= length) {
        dcm_error_set(error, DCM_ERROR_CODE_INVALID,
                      "Removing item from Sequence failed",
                      "Index %i exceeds length of sequence %i",
                      index, length);
        return;
    }
    utarray_erase(seq->items, index, 1);
}


uint32_t dcm_sequence_count(const DcmSequence *seq)
{
    uint32_t length = utarray_len(seq->items);
    return length;
}


void dcm_sequence_lock(DcmSequence *seq)
{
    seq->is_locked = true;
}


bool dcm_sequence_is_locked(const DcmSequence *seq)
{
    return seq->is_locked;
}


void dcm_sequence_destroy(DcmSequence *seq)
{
    if (seq) {
        utarray_free(seq->items);
        seq->items = NULL;
        free(seq);
        seq = NULL;
    }
}


// Frames

DcmFrame *dcm_frame_create(DcmError **error, 
                           uint32_t number,
                           const char *data,
                           uint32_t length,
                           uint16_t rows,
                           uint16_t columns,
                           uint16_t samples_per_pixel,
                           uint16_t bits_allocated,
                           uint16_t bits_stored,
                           uint16_t pixel_representation,
                           uint16_t planar_configuration,
                           const char *photometric_interpretation,
                           const char *transfer_syntax_uid)
{
    if (data == NULL || length == 0) {
        dcm_error_set(error, DCM_ERROR_CODE_INVALID,
                      "Constructing Frame Item failed",
                      "Pixel data cannot be empty");
        return NULL;
    }

    if (bits_allocated != 1 && bits_allocated % 8 != 0) {
        dcm_error_set(error, DCM_ERROR_CODE_INVALID,
                      "Constructing Frame Item failed",
                      "Wrong number of bits allocated");
        return NULL;
    }

    if (bits_stored != 1 && bits_stored % 8 != 0) {
        dcm_error_set(error, DCM_ERROR_CODE_INVALID,
                      "Constructing Frame Item failed",
                      "Wrong number of bits stored");
        return NULL;
    }

    if (pixel_representation != 0 && pixel_representation != 1) {
        dcm_error_set(error, DCM_ERROR_CODE_INVALID,
                      "Constructing Frame Item failed",
                      "Wrong pixel representation");
        return NULL;
    }

    if (planar_configuration != 0 && planar_configuration != 1) {
        dcm_error_set(error, DCM_ERROR_CODE_INVALID,
                      "Constructing Frame Item failed",
                      "Wrong planar configuration");
        return NULL;
    }

    DcmFrame *frame = DCM_NEW(error, DcmFrame);
    if (frame == NULL) {
        return NULL;
    }
    frame->number = number;
    frame->data = data;
    frame->length = length;
    frame->rows = rows;
    frame->columns = columns;
    frame->samples_per_pixel = samples_per_pixel;
    frame->bits_allocated = bits_allocated;
    frame->bits_stored = bits_stored;
    frame->high_bit = bits_stored - 1;
    frame->pixel_representation = pixel_representation;
    frame->planar_configuration = planar_configuration;
    frame->photometric_interpretation = 
        dcm_strdup(error, photometric_interpretation);
    if (frame->photometric_interpretation == NULL) {
        dcm_frame_destroy(frame);
        return NULL;
    }

    frame->transfer_syntax_uid = dcm_strdup(error, transfer_syntax_uid);
    if (frame->transfer_syntax_uid == NULL) {
        dcm_frame_destroy(frame);
        return NULL;
    }

    return frame;
}


uint32_t dcm_frame_get_number(const DcmFrame *frame)
{
    assert(frame);
    return frame->number;
}

uint32_t dcm_frame_get_length(const DcmFrame *frame)
{
    assert(frame);
    return frame->length;
}

uint16_t dcm_frame_get_rows(const DcmFrame *frame)
{
    assert(frame);
    return frame->rows;
}

uint16_t dcm_frame_get_columns(const DcmFrame *frame)
{
    assert(frame);
    return frame->columns;
}

uint16_t dcm_frame_get_samples_per_pixel(const DcmFrame *frame)
{
    assert(frame);
    return frame->samples_per_pixel;
}

uint16_t dcm_frame_get_bits_allocated(const DcmFrame *frame)
{
    assert(frame);
    return frame->bits_allocated;
}

uint16_t dcm_frame_get_bits_stored(const DcmFrame *frame)
{
    assert(frame);
    return frame->bits_stored;
}

uint16_t dcm_frame_get_high_bit(const DcmFrame *frame)
{
    assert(frame);
    return frame->high_bit;
}

uint16_t dcm_frame_get_pixel_representation(const DcmFrame *frame)
{
    assert(frame);
    return frame->pixel_representation;
}

uint16_t dcm_frame_get_planar_configuration(const DcmFrame *frame)
{
    assert(frame);
    return frame->planar_configuration;
}

const char *dcm_frame_get_photometric_interpretation(const DcmFrame *frame)
{
    assert(frame);
    return frame->photometric_interpretation;
}

const char *dcm_frame_get_transfer_syntax_uid(const DcmFrame *frame)
{
    assert(frame);
    return frame->transfer_syntax_uid;
}


const char *dcm_frame_get_value(const DcmFrame *frame)
{
    assert(frame);
    return frame->data;
}


void dcm_frame_destroy(DcmFrame *frame)
{
    if (frame) {
        if (frame->data) {
            free((char*)frame->data);
        }
        if (frame->photometric_interpretation) {
            free((char*)frame->photometric_interpretation);
        }
        if (frame->transfer_syntax_uid) {
            free((char*)frame->transfer_syntax_uid);
        }
        free(frame);
        frame = NULL;
    }
}


// Basic Offset Table

DcmBOT *dcm_bot_create(DcmError **error, 
                       ssize_t *offsets, uint32_t num_frames, 
                       ssize_t first_frame_offset)
{
    if (num_frames == 0) {
        dcm_error_set(error, DCM_ERROR_CODE_INVALID,
                      "Constructing Basic Offset Table failed",
                      "Expected offsets of %ld Frame Items",
                      num_frames);
        free(offsets);
        return NULL;
    }

    if (offsets == NULL) {
        dcm_error_set(error, DCM_ERROR_CODE_INVALID,
                      "Constructing Basic Offset Table failed",
                      "No offsets were provided");
        return NULL;
    }
    DcmBOT *bot = DCM_NEW(error, DcmBOT);
    if (bot == NULL) {
        free(offsets);
        return NULL;
    }
    bot->num_frames = num_frames;
    bot->offsets = offsets;
    bot->first_frame_offset = first_frame_offset;
    return bot;
}


void dcm_bot_print(const DcmBOT *bot)
{
    assert(bot);
    uint32_t i;

    printf("[");
    for(i = 0; i < bot->num_frames; i++) {
        printf("%zd", bot->offsets[i] + bot->first_frame_offset);
        if (i == (bot->num_frames - 1)) {
            printf("]\n");
        } else {
            printf(",");
        }
    }
}


uint32_t dcm_bot_get_num_frames(const DcmBOT *bot)
{
    assert(bot);
    return bot->num_frames;
}


ssize_t dcm_bot_get_frame_offset(const DcmBOT *bot, uint32_t number)
{
    assert(bot);
    assert(number > 0 && number < bot->num_frames + 1);
    uint32_t index = number - 1;
    return bot->offsets[index] + bot->first_frame_offset;
}


void dcm_bot_destroy(DcmBOT *bot)
{
    if (bot) {
        if (bot->offsets) {
            free(bot->offsets);
        }
        free(bot);
        bot = NULL;
    }
}


bool dcm_is_encapsulated_transfer_syntax(const char *transfer_syntax_uid) 
{
    return 
        strcmp(transfer_syntax_uid, "1.2.840.10008.1.2") != 0 &&
        strcmp(transfer_syntax_uid, "1.2.840.10008.1.2.1") != 0 &&
        strcmp(transfer_syntax_uid, "1.2.840.10008.1.2.1.99") != 0 &&
        strcmp(transfer_syntax_uid, "1.2.840.10008.1.2.2") != 0;
}
