/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_expat — expat smoke test for our OS.
 *
 * Verifies that libexpat.so loads and the basic API works:
 *   1. XML_ParserCreate / Free
 *   2. Parse a simple XML document
 *   3. Parse nested XML with attributes and character data
 *   4. Error handling (malformed XML → XML_GetErrorCode)
 */

#include <expat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

/* ---- helpers ---- */

static int start_elem_count;
static int end_elem_count;
static int char_data_count;

static void XMLCALL start_element(void *userData, const XML_Char *name,
                                  const XML_Char **atts) {
  (void)userData;
  (void)name;
  (void)atts;
  start_elem_count++;
}

static void XMLCALL end_element(void *userData, const XML_Char *name) {
  (void)userData;
  (void)name;
  end_elem_count++;
}

static void XMLCALL char_data(void *userData, const XML_Char *s, int len) {
  (void)userData;
  (void)s;
  (void)len;
  char_data_count++;
}

/* ---- Case 1: XML_ParserCreate / Free round-trip ---- */

void test_parser_create_free(void) {
  XML_Parser p = XML_ParserCreate(NULL);
  TEST_ASSERT_NOT_NULL(p);
  XML_ParserFree(p);
}

/* ---- Case 2: Parse a minimal XML document ---- */

void test_parse_minimal(void) {
  const char *xml = "<root/>";
  XML_Parser p = XML_ParserCreate(NULL);
  TEST_ASSERT_NOT_NULL(p);

  enum XML_Status s = XML_Parse(p, xml, (int)strlen(xml), XML_TRUE);
  TEST_ASSERT_EQUAL_INT(XML_STATUS_OK, s);

  XML_ParserFree(p);
}

/* ---- Case 3: Parse nested XML with callbacks ---- */

void test_parse_nested_callbacks(void) {
  const char *xml = "<catalog><book id=\"1\">Title</book></catalog>";

  start_elem_count = 0;
  end_elem_count = 0;
  char_data_count = 0;

  XML_Parser p = XML_ParserCreate(NULL);
  TEST_ASSERT_NOT_NULL(p);

  XML_SetElementHandler(p, start_element, end_element);
  XML_SetCharacterDataHandler(p, char_data);

  enum XML_Status s = XML_Parse(p, xml, (int)strlen(xml), XML_TRUE);
  TEST_ASSERT_EQUAL_INT(XML_STATUS_OK, s);

  /* <catalog> <book> = 2 start, 2 end; "Title" = 1 char_data */
  TEST_ASSERT_EQUAL_INT(2, start_elem_count);
  TEST_ASSERT_EQUAL_INT(2, end_elem_count);
  TEST_ASSERT_EQUAL_INT(1, char_data_count);

  XML_ParserFree(p);
}

/* ---- Case 4: Error handling (malformed XML) ---- */

void test_parse_error(void) {
  const char *xml = "<root><unclosed>"; /* malformed — no matching end tag */

  XML_Parser p = XML_ParserCreate(NULL);
  TEST_ASSERT_NOT_NULL(p);

  enum XML_Status s = XML_Parse(p, xml, (int)strlen(xml), XML_TRUE);
  TEST_ASSERT_EQUAL_INT(XML_STATUS_ERROR, s);

  enum XML_Error err = XML_GetErrorCode(p);
  /* For <root><unclosed> with XML_TRUE (final), expat returns
   * XML_ERROR_TAG_MISMATCH or XML_ERROR_UNCLOSED_TOKEN. Accept any error. */
  TEST_ASSERT(XML_ERROR_NONE != err);

  XML_ParserFree(p);
}

/* ---- Case 5: XML_ParserReset ---- */

void test_parser_reset(void) {
  XML_Parser p = XML_ParserCreate(NULL);
  TEST_ASSERT_NOT_NULL(p);

  /* Parse valid document first */
  const char *xml1 = "<doc/>";
  TEST_ASSERT_EQUAL_INT(XML_STATUS_OK,
                        XML_Parse(p, xml1, (int)strlen(xml1), XML_TRUE));

  /* Reset and parse another document */
  TEST_ASSERT_EQUAL_INT(XML_TRUE, XML_ParserReset(p, NULL));
  const char *xml2 = "<root><item/></root>";
  TEST_ASSERT_EQUAL_INT(XML_STATUS_OK,
                        XML_Parse(p, xml2, (int)strlen(xml2), XML_TRUE));

  XML_ParserFree(p);
}

/* ---- main ---- */

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  UNITY_BEGIN();
  RUN_TEST(test_parser_create_free);
  RUN_TEST(test_parse_minimal);
  RUN_TEST(test_parse_nested_callbacks);
  RUN_TEST(test_parse_error);
  RUN_TEST(test_parser_reset);
  return UNITY_END();
}
