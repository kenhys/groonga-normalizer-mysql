/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2013  Kouhei Sutou <kou@clear-code.com>

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Library General Public
  License as published by the Free Software Foundation; version 2
  of the License.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Library General Public License for more details.

  You should have received a copy of the GNU Library General Public
  License along with this library; if not, write to the Free
  Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston,
  MA 02110-1301, USA
*/

#include <groonga/normalizer.h>
#include <groonga/nfkc.h>

#include "mysql_general_ci_table.h"
#include "mysql_unicode_ci_table.h"
#include "mysql_unicode_ci_except_kana_ci_kana_with_voiced_sound_mark_table.h"

#ifdef __GNUC__
#  define GNUC_UNUSED __attribute__((__unused__))
#else
#  define GNUC_UNUSED
#endif

#ifdef _MSC_VER
#  define inline _inline
#endif

typedef grn_bool (*normalizer_func)(grn_ctx *ctx,
                                    const char *utf8,
                                    int *character_length,
                                    int rest_length,
                                    uint32_t **normalize_table,
                                    char *normalized,
                                    unsigned int *normalized_length_in_bytes,
                                    unsigned int *normalized_n_characters);

static inline unsigned int
unichar_to_utf8(uint32_t unichar, char *output)
{
  unsigned int n_bytes;

  if (unichar < 0x80) {
    output[0] = unichar;
    n_bytes = 1;
  } else if (unichar < 0x0800) {
    output[0] = ((unichar >> 6) & 0x1f) | 0xc0;
    output[1] = (unichar & 0x3f) | 0x80;
    n_bytes = 2;
  } else if (unichar < 0x10000) {
    output[0] = (unichar >> 12) | 0xe0;
    output[1] = ((unichar >> 6) & 0x3f) | 0x80;
    output[2] = (unichar & 0x3f) | 0x80;
    n_bytes = 3;
  } else if (unichar < 0x200000) {
    output[0] = (unichar >> 18) | 0xf0;
    output[1] = ((unichar >> 12) & 0x3f) | 0x80;
    output[2] = ((unichar >> 6) & 0x3f) | 0x80;
    output[3] = (unichar & 0x3f) | 0x80;
    n_bytes = 4;
  } else if (unichar < 0x4000000) {
    output[0] = (unichar >> 24) | 0xf8;
    output[1] = ((unichar >> 18) & 0x3f) | 0x80;
    output[2] = ((unichar >> 12) & 0x3f) | 0x80;
    output[3] = ((unichar >> 6) & 0x3f) | 0x80;
    output[4] = (unichar & 0x3f) | 0x80;
    n_bytes = 5;
  } else {
    output[0] = (unichar >> 30) | 0xfc;
    output[1] = ((unichar >> 24) & 0x3f) | 0x80;
    output[2] = ((unichar >> 18) & 0x3f) | 0x80;
    output[3] = ((unichar >> 12) & 0x3f) | 0x80;
    output[4] = ((unichar >> 6) & 0x3f) | 0x80;
    output[5] = (unichar & 0x3f) | 0x80;
    n_bytes = 6;
  }

  return n_bytes;
}

static inline uint32_t
utf8_to_unichar(const char *utf8, int byte_size)
{
  uint32_t unichar;
  const unsigned char *bytes = (const unsigned char *)utf8;

  switch (byte_size) {
  case 1 :
    unichar = bytes[0] & 0x7f;
    break;
  case 2 :
    unichar = ((bytes[0] & 0x1f) << 6) + (bytes[1] & 0x3f);
    break;
  case 3 :
    unichar =
      ((bytes[0] & 0x0f) << 12) +
      ((bytes[1] & 0x3f) << 6) +
      ((bytes[2] & 0x3f));
    break;
  case 4 :
    unichar =
      ((bytes[0] & 0x07) << 18) +
      ((bytes[1] & 0x3f) << 12) +
      ((bytes[2] & 0x3f) << 6) +
      ((bytes[3] & 0x3f));
    break;
  case 5 :
    unichar =
      ((bytes[0] & 0x03) << 24) +
      ((bytes[1] & 0x3f) << 18) +
      ((bytes[2] & 0x3f) << 12) +
      ((bytes[3] & 0x3f) << 6) +
      ((bytes[4] & 0x3f));
    break;
  case 6 :
    unichar =
      ((bytes[0] & 0x01) << 30) +
      ((bytes[1] & 0x3f) << 24) +
      ((bytes[2] & 0x3f) << 18) +
      ((bytes[3] & 0x3f) << 12) +
      ((bytes[4] & 0x3f) << 6) +
      ((bytes[5] & 0x3f));
    break;
  default :
    unichar = 0;
    break;
  }

  return unichar;
}

static inline void
decompose_character(const char *rest, int character_length,
                    int *page, uint32_t *low_code)
{
  switch (character_length) {
  case 1 :
    *page = 0x00;
    *low_code = rest[0] & 0x7f;
    break;
  case 2 :
    *page = (rest[0] & 0x1c) >> 2;
    *low_code = ((rest[0] & 0x03) << 6) + (rest[1] & 0x3f);
    break;
  case 3 :
    *page = ((rest[0] & 0x0f) << 4) + ((rest[1] & 0x3c) >> 2);
    *low_code = ((rest[1] & 0x03) << 6) + (rest[2] & 0x3f);
    break;
  case 4 :
    *page =
      ((rest[0] & 0x07) << 10) +
      ((rest[1] & 0x3f) << 4) +
      ((rest[2] & 0x3c) >> 2);
    *low_code = ((rest[1] & 0x03) << 6) + (rest[2] & 0x3f);
    break;
  default :
    *page = -1;
    *low_code = 0x00;
    break;
  }
}

static inline void
normalize_character(const char *utf8, int character_length,
                    uint32_t **normalize_table,
                    char *normalized,
                    unsigned int *normalized_length_in_bytes,
                    unsigned int *normalized_n_characters)
{
  int page;
  uint32_t low_code;
  decompose_character(utf8, character_length, &page, &low_code);
  if ((0x00 <= page && page <= 0xff) && normalize_table[page]) {
    uint32_t normalized_code;
    unsigned int n_bytes;
    normalized_code = normalize_table[page][low_code];
    if (normalized_code != 0) {
      n_bytes = unichar_to_utf8(normalized_code,
                                normalized + *normalized_length_in_bytes);
      *normalized_length_in_bytes += n_bytes;
    }
  } else {
    int i;
    for (i = 0; i < character_length; i++) {
      normalized[*normalized_length_in_bytes + i] = utf8[i];
    }
    *normalized_length_in_bytes += character_length;
  }
  (*normalized_n_characters)++;
}

static void
normalize(grn_ctx *ctx, grn_obj *string, uint32_t **normalize_table,
          normalizer_func custom_normalizer)
{
  const char *original, *rest;
  unsigned int original_length_in_bytes, rest_length;
  char *normalized;
  unsigned int normalized_length_in_bytes = 0;
  unsigned int normalized_n_characters = 0;
  unsigned char *types = NULL;
  unsigned char *current_type = NULL;
  grn_encoding encoding;
  int flags;
  grn_bool remove_blank_p;

  encoding = grn_string_get_encoding(ctx, string);
  flags = grn_string_get_flags(ctx, string);
  remove_blank_p = flags & GRN_STRING_REMOVE_BLANK;
  grn_string_get_original(ctx, string, &original, &original_length_in_bytes);
  {
    unsigned int max_normalized_length_in_bytes = original_length_in_bytes;
    normalized = GRN_PLUGIN_MALLOC(ctx, max_normalized_length_in_bytes);
  }
  if (flags & GRN_STRING_WITH_TYPES) {
    unsigned int max_normalized_n_characters = original_length_in_bytes;
    types = GRN_PLUGIN_MALLOC(ctx, max_normalized_n_characters);
    current_type = types;
  }
  rest = original;
  rest_length = original_length_in_bytes;
  while (rest_length > 0) {
    int character_length;

    character_length = grn_plugin_charlen(ctx, rest, rest_length, encoding);
    if (character_length == 0) {
      break;
    }

    if (remove_blank_p && character_length == 1 && rest[0] == ' ') {
      if (current_type > types) {
        current_type[-1] |= GRN_CHAR_BLANK;
      }
    } else {
      grn_bool custom_normalized = GRN_FALSE;
      if (custom_normalizer) {
        custom_normalized = custom_normalizer(ctx,
                                              rest,
                                              &character_length,
                                              rest_length - character_length,
                                              normalize_table,
                                              normalized,
                                              &normalized_length_in_bytes,
                                              &normalized_n_characters);
      }
      if (!custom_normalized) {
        normalize_character(rest, character_length, normalize_table,
                            normalized,
                            &normalized_length_in_bytes,
                            &normalized_n_characters);
      }
      if (current_type) {
        char *current_normalized;
        current_normalized =
          normalized + normalized_length_in_bytes - character_length;
        current_type[0] =
          grn_nfkc_char_type((unsigned char *)current_normalized);
        current_type++;
      }
    }

    rest += character_length;
    rest_length -= character_length;
  }

  if (rest_length == 0) {
    grn_string_set_normalized(ctx,
                              string,
                              normalized,
                              normalized_length_in_bytes,
                              normalized_n_characters);
    grn_string_set_types(ctx, string, types);
  } else {
    /* TODO: report error */
    grn_string_set_normalized(ctx,
                              string,
                              normalized,
                              0,
                              0);
  }
}

static grn_obj *
mysql_general_ci_next(GNUC_UNUSED grn_ctx *ctx,
                      GNUC_UNUSED int nargs,
                      grn_obj **args,
                      GNUC_UNUSED grn_user_data *user_data)
{
  grn_obj *string = args[0];
  grn_encoding encoding;

  encoding = grn_string_get_encoding(ctx, string);
  if (encoding != GRN_ENC_UTF8) {
    GRN_PLUGIN_ERROR(ctx,
                     GRN_FUNCTION_NOT_IMPLEMENTED,
                     "[normalizer][mysql-general-ci] "
                     "UTF-8 encoding is only supported: %s",
                     grn_encoding_to_string(encoding));
    return NULL;
  }
  normalize(ctx, string, general_ci_table, NULL);
  return NULL;
}

static grn_obj *
mysql_unicode_ci_next(GNUC_UNUSED grn_ctx *ctx,
                      GNUC_UNUSED int nargs,
                      grn_obj **args,
                      GNUC_UNUSED grn_user_data *user_data)
{
  grn_obj *string = args[0];
  grn_encoding encoding;

  encoding = grn_string_get_encoding(ctx, string);
  if (encoding != GRN_ENC_UTF8) {
    GRN_PLUGIN_ERROR(ctx,
                     GRN_FUNCTION_NOT_IMPLEMENTED,
                     "[normalizer][mysql-unicode-ci] "
                     "UTF-8 encoding is only supported: %s",
                     grn_encoding_to_string(encoding));
    return NULL;
  }
  normalize(ctx, string, unicode_ci_table, NULL);
  return NULL;
}

#define HALFWIDTH_KATAKANA_LETTER_KA 0xff76
#define HALFWIDTH_KATAKANA_LETTER_KI 0xff77
#define HALFWIDTH_KATAKANA_LETTER_KU 0xff78
#define HALFWIDTH_KATAKANA_LETTER_KE 0xff79
#define HALFWIDTH_KATAKANA_LETTER_KO 0xff7a

#define HALFWIDTH_KATAKANA_LETTER_SA 0xff7b
#define HALFWIDTH_KATAKANA_LETTER_SI 0xff7c
#define HALFWIDTH_KATAKANA_LETTER_SU 0xff7d
#define HALFWIDTH_KATAKANA_LETTER_SE 0xff7e
#define HALFWIDTH_KATAKANA_LETTER_SO 0xff7f

#define HALFWIDTH_KATAKANA_LETTER_TA 0xff80
#define HALFWIDTH_KATAKANA_LETTER_TI 0xff81
#define HALFWIDTH_KATAKANA_LETTER_TU 0xff82
#define HALFWIDTH_KATAKANA_LETTER_TE 0xff83
#define HALFWIDTH_KATAKANA_LETTER_TO 0xff84

#define HALFWIDTH_KATAKANA_LETTER_HA 0xff8a
#define HALFWIDTH_KATAKANA_LETTER_HI 0xff8b
#define HALFWIDTH_KATAKANA_LETTER_HU 0xff8c
#define HALFWIDTH_KATAKANA_LETTER_HE 0xff8d
#define HALFWIDTH_KATAKANA_LETTER_HO 0xff8e

#define HALFWIDTH_KATAKANA_VOICED_SOUND_MARK      0xff9e
#define HALFWIDTH_KATAKANA_SEMI_VOICED_SOUND_MARK 0xff9f

#define HIRAGANA_LETTER_KA                0x304b
#define HIRAGANA_VOICED_SOUND_MARK_OFFSET 1
#define HIRAGANA_VOICED_SOUND_MARK_GAP    2

#define HIRAGANA_LETTER_HA         0x306f
#define HIRAGANA_HA_LINE_BA_OFFSET 1
#define HIRAGANA_HA_LINE_PA_OFFSET 2
#define HIRAGANA_HA_LINE_GAP       3

static grn_bool
normalize_halfwidth_katakana_with_voiced_sound_mark(
  grn_ctx *ctx,
  const char *utf8,
  int *character_length,
  int rest_length,
  GNUC_UNUSED uint32_t **normalize_table,
  char *normalized,
  unsigned int *normalized_length_in_bytes,
  unsigned int *normalized_n_characters)
{
  grn_bool custom_normalized = GRN_FALSE;
  grn_bool is_voiced_sound_markable_halfwidth_katakana = GRN_FALSE;
  grn_bool is_semi_voiced_sound_markable_halfwidth_katakana = GRN_FALSE;
  grn_bool is_ha_line = GRN_FALSE;
  uint32_t unichar;

  if (*character_length != 3) {
    return GRN_FALSE;
  }
  if (rest_length < 3) {
    return GRN_FALSE;
  }

  unichar = utf8_to_unichar(utf8, *character_length);
  if (HALFWIDTH_KATAKANA_LETTER_KA <= unichar &&
      unichar <= HALFWIDTH_KATAKANA_LETTER_TO) {
    is_voiced_sound_markable_halfwidth_katakana = GRN_TRUE;
  } else if (HALFWIDTH_KATAKANA_LETTER_HA <= unichar &&
             unichar <= HALFWIDTH_KATAKANA_LETTER_HO) {
    is_voiced_sound_markable_halfwidth_katakana = GRN_TRUE;
    is_semi_voiced_sound_markable_halfwidth_katakana = GRN_TRUE;
    is_ha_line = GRN_TRUE;
  }

  if (!is_voiced_sound_markable_halfwidth_katakana &&
      !is_semi_voiced_sound_markable_halfwidth_katakana) {
    return GRN_FALSE;
  }

  {
    int next_character_length;
    uint32_t next_unichar;
    next_character_length = grn_plugin_charlen(ctx,
                                               utf8 + *character_length,
                                               rest_length,
                                               GRN_ENC_UTF8);
    if (next_character_length != 3) {
      return GRN_FALSE;
    }
    next_unichar = utf8_to_unichar(utf8 + *character_length,
                                   next_character_length);
    if (next_unichar == HALFWIDTH_KATAKANA_VOICED_SOUND_MARK) {
      if (is_voiced_sound_markable_halfwidth_katakana) {
        unsigned int n_bytes;
        if (is_ha_line) {
          n_bytes = unichar_to_utf8(HIRAGANA_LETTER_HA +
                                    HIRAGANA_HA_LINE_BA_OFFSET +
                                    ((unichar - HALFWIDTH_KATAKANA_LETTER_HA) *
                                     HIRAGANA_HA_LINE_GAP),
                                    normalized + *normalized_length_in_bytes);
        } else {
          int small_tu_offset = 0;
          if (HALFWIDTH_KATAKANA_LETTER_TU <= unichar &&
              unichar <= HALFWIDTH_KATAKANA_LETTER_TO) {
            small_tu_offset = 1;
          }
          n_bytes = unichar_to_utf8(HIRAGANA_LETTER_KA +
                                    HIRAGANA_VOICED_SOUND_MARK_OFFSET +
                                    small_tu_offset +
                                    ((unichar - HALFWIDTH_KATAKANA_LETTER_KA) *
                                     HIRAGANA_VOICED_SOUND_MARK_GAP),
                                    normalized + *normalized_length_in_bytes);
        }
        *character_length += next_character_length;
        *normalized_length_in_bytes += n_bytes;
        (*normalized_n_characters)++;
        custom_normalized = GRN_TRUE;
      }
    } else if (next_unichar == HALFWIDTH_KATAKANA_SEMI_VOICED_SOUND_MARK) {
      if (is_semi_voiced_sound_markable_halfwidth_katakana) {
        unsigned int n_bytes;
        n_bytes = unichar_to_utf8(HIRAGANA_LETTER_HA +
                                  HIRAGANA_HA_LINE_PA_OFFSET +
                                  ((unichar - HALFWIDTH_KATAKANA_LETTER_HA) *
                                   HIRAGANA_HA_LINE_GAP),
                                  normalized + *normalized_length_in_bytes);
        *character_length += next_character_length;
        *normalized_length_in_bytes += n_bytes;
        (*normalized_n_characters)++;
        custom_normalized = GRN_TRUE;
      }
    }
  }

  return custom_normalized;
}

static grn_obj *
mysql_unicode_ci_except_kana_ci_kana_with_voiced_sound_mark_next(
  GNUC_UNUSED grn_ctx *ctx,
  GNUC_UNUSED int nargs,
  grn_obj **args,
  GNUC_UNUSED grn_user_data *user_data)
{
  grn_obj *string = args[0];
  grn_encoding encoding;

  encoding = grn_string_get_encoding(ctx, string);
  if (encoding != GRN_ENC_UTF8) {
    GRN_PLUGIN_ERROR(ctx,
                     GRN_FUNCTION_NOT_IMPLEMENTED,
                     "[normalizer]"
                     "[mysql-unicode-ci-except-kana-ci-kana-with-voiced-sound-mark] "
                     "UTF-8 encoding is only supported: %s",
                     grn_encoding_to_string(encoding));
    return NULL;
  }
  normalize(ctx, string,
            unicode_ci_except_kana_ci_kana_with_voiced_sound_mark_table,
            normalize_halfwidth_katakana_with_voiced_sound_mark);
  return NULL;
}

grn_rc
GRN_PLUGIN_INIT(grn_ctx *ctx)
{
  return ctx->rc;
}

grn_rc
GRN_PLUGIN_REGISTER(grn_ctx *ctx)
{
  grn_normalizer_register(ctx, "NormalizerMySQLGeneralCI", -1,
                          NULL, mysql_general_ci_next, NULL);
  grn_normalizer_register(ctx, "NormalizerMySQLUnicodeCI", -1,
                          NULL, mysql_unicode_ci_next, NULL);
  grn_normalizer_register(ctx,
                          "NormalizerMySQLUnicodeCI"
                          "Except"
                          "KanaCI"
                          "KanaWithVoicedSoundMark",
                          -1,
                          NULL,
                          mysql_unicode_ci_except_kana_ci_kana_with_voiced_sound_mark_next,
                          NULL);
  return GRN_SUCCESS;
}

grn_rc
GRN_PLUGIN_FIN(GNUC_UNUSED grn_ctx *ctx)
{
  return GRN_SUCCESS;
}
