/**************************************************************************\
 *
 *  This file is part of the Coin 3D visualization library.
 *  Copyright (C) 1998-2007 by Systems in Motion.  All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  ("GPL") version 2 as published by the Free Software Foundation.
 *  See the file LICENSE.GPL at the root directory of this source
 *  distribution for additional information about the GNU GPL.
 *
 *  For using Coin with software that can not be combined with the GNU
 *  GPL, and for taking advantage of the additional benefits of our
 *  support services, please contact Systems in Motion about acquiring
 *  a Coin Professional Edition License.
 *
 *  See http://www.coin3d.org/ for more information.
 *
 *  Systems in Motion, Postboks 1283, Pirsenteret, 7462 Trondheim, NORWAY.
 *  http://www.sim.no/  sales@sim.no  coin-support@coin3d.org
 *
\**************************************************************************/

#include <Inventor/C/XML/document.h>
#include "documentp.h"

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif // HAVE_CONFIG_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <boost/scoped_array.hpp>

#include <coindefs.h>
#include <Inventor/C/XML/element.h>
#include <Inventor/C/XML/attribute.h>
#include <Inventor/C/XML/path.h>
#include <Inventor/C/XML/parser.h>
#include <Inventor/lists/SbList.h>
#include <Inventor/SbString.h>

#include "expat/expat.h"
#include "utils.h"
#include "elementp.h"

// #define DEV_DEBUG 1

/*!
  \page xmlparsing XML Parsing with Coin

  For Coin 3.0, we added an XML parser to Coin.  This document describes
  how it can be used for generic purposes.

  Why another XML parser, you might ask?  First of all, the XML parser
  is actually a third-party parser, expat.  Coin needed one, and many
  Coin-dependent projects needed one as well.  We therefore needed to
  expose an API for it.  However, integrating a 3rd-party parser into
  Coin, we can not expose its API directly, or other projects also
  using Expat would get conflicts.  We therefore needed to expose the
  XML API with a unique API, hence the API you see here.  It is based
  on a XML DOM API we use(d) in a couple of other projects, but it has
  been tweaked to fit into Coin and to be wrapped over Expat (the
  original implementation just used flex).

  The XML parser is both a streaming parser and a DOM parser.  Being a
  streaming parser means that documents can be read in without having
  to be fully contained in memory.  When used as a DOM parser, the
  whole document is fully parsed in first, and then inspected by
  client code by traversing the DOM.  The two modes can actually be
  mixed arbitrarily if ending up with a partial DOM sounds useful.

  The XML parser has both a C API and a C++ API.  The C++ API is just
  a wrapper around the C API, and only serves as convenience if you
  prefer to read/write C++ code (which is tighter) over more verbose
  C code.

  The C API naming convention may look a bit strange, unless you have
  written libraries to be wrapped for scheme/lisp-like languages
  before.  Then you might be familiar with the convention of suffixing
  your functions based on their behaviour/usage meaning.  Mutating
  functions are suffixed with "!", or "_x" for (eXclamation point),
  and predicates are suffixed with "?", or "_p" in C.

  The simplest way to use the XML parser is to just call
  cc_xml_read_file(filename) and then traverse the DOM model through
  using cc_xml_doc_get_root(), cc_xml_elt_get_child(), and
  cc_xml_elt_get_attr().

  \sa XML, cc_xml_doc, cc_xml_elt, cc_xml_attr
*/

/*!
  \defgroup XML XML related functions and objects
*/

// *************************************************************************

/*!
  \typedef struct cc_xml_doc cc_xml_doc

  This type is an opaque container object type for an XML document structure,
  and also the interface for configuring the parsing and writing code.

  \ingroup XML
*/

struct cc_xml_doc {
  XML_Parser parser;

  cc_xml_filter_cb * filtercb;
  void * filtercbdata;
  
  // document type
  
  char * xmlversion;
  char * xmlencoding;

  char * filename;
  cc_xml_elt * root;
  cc_xml_elt * current;

  SbList<cc_xml_elt *> parsestack;
};

// *************************************************************************
// internal functions

namespace {

void
cc_xml_doc_expat_element_start_handler_cb(void * userdata, const XML_Char * elementtype, const XML_Char ** attributes)
{
  XML_Parser parser = static_cast<XML_Parser>(userdata);
  cc_xml_doc * doc = static_cast<cc_xml_doc *>(XML_GetUserData(parser));

  cc_xml_elt * elt = cc_xml_elt_new_from_data(elementtype, NULL);
  assert(elt);

  // FIXME: check if attribute values are automatically dequoted or not...
  // (dequote if not)
  if (attributes) {
    for (int c = 0; attributes[c] != NULL; c += 2) {
      cc_xml_attr * attr = cc_xml_attr_new_from_data(attributes[c], attributes[c+1]);
      cc_xml_elt_set_attribute_x(elt, attr);
    }
  }

  if (doc->parsestack.getLength() > 0) {
    cc_xml_elt * parent = doc->parsestack[doc->parsestack.getLength()-1];
    cc_xml_elt_add_child_x(parent, elt);
  }

  if ((doc->parsestack.getLength() == 0) && (doc->root == NULL)) {
    cc_xml_doc_set_root_x(doc, elt);
  }

  doc->parsestack.push(elt);

  if (doc->filtercb) {
    doc->filtercb(doc->filtercbdata, doc, elt, TRUE);
  }
}

void
cc_xml_doc_expat_element_end_handler_cb(void * userdata, const XML_Char * element)
{
  XML_Parser parser = static_cast<XML_Parser>(userdata);
  cc_xml_doc * doc = static_cast<cc_xml_doc *>(XML_GetUserData(parser));

  const int stackdepth = doc->parsestack.getLength();
  if (stackdepth == 0) {
    // flag error
    return;
  }

  cc_xml_elt * topelt = doc->parsestack.pop();
  if (strcmp(cc_xml_elt_get_type(topelt), element) != 0) {
    // this means XML input is closing a tag that was not the one opened at
    // this level. flag error
  }

  if (doc->filtercb) {
    switch (doc->filtercb(doc->filtercbdata, doc, topelt, FALSE)) {
    case DISCARD:
      {
        cc_xml_elt * parent = cc_xml_elt_get_parent(topelt);
        if (parent) {
          cc_xml_elt_remove_child_x(parent, topelt);
          cc_xml_elt_delete_x(topelt);
        } else {
          if (topelt == doc->root) {
            cc_xml_doc_set_root_x(doc, NULL);
            cc_xml_elt_delete_x(topelt);
          } else {
            assert(!"invalid case - investigate");
          }
        }
      }
      break;
    case KEEP:
      break;
    default:
      assert(!"invalid filter choice returned from client code");
      break;
    }
  }
}

SbBool
cc_xml_is_all_whitespace_p(const char * strptr)
{
  while (*strptr) {
    switch (*strptr) {
    case ' ':
    case '\t':
    case '\n':
    case '\r':
      break;
    default:
      return FALSE;
    }
    ++strptr;
  }
  return TRUE;
}

void
cc_xml_doc_expat_character_data_handler_cb(void * userdata, const XML_Char * cdata, int len)
{
#ifdef DEV_DEBUG
  fprintf(stdout, "cc_xml_doc_expat_character_data_handler_cb()\n");
#endif // DEV_DEBUG

  XML_Parser parser = static_cast<XML_Parser>(userdata);
  cc_xml_doc * doc = static_cast<cc_xml_doc *>(XML_GetUserData(parser));

  cc_xml_elt * elt = cc_xml_elt_new();
  assert(elt);

  // need a temporary buffer for the cdata to make a nullterminated string.
  boost::scoped_array<char> buffer;
  buffer.reset(new char [len + 1]);
  memcpy(buffer.get(), cdata, len);
  buffer[len] = '\0';
  cc_xml_elt_set_type_x(elt, COIN_XML_CDATA_TYPE);
  cc_xml_elt_set_cdata_x(elt, buffer.get());

  if (cc_xml_is_all_whitespace_p(buffer.get())) {
    return;
  }

  if (doc->parsestack.getLength() > 0) {
    cc_xml_elt * parent = doc->parsestack[doc->parsestack.getLength()-1];
    cc_xml_elt_add_child_x(parent, elt);
  }

  if (doc->filtercb) {
    doc->filtercb(doc->filtercbdata, doc, elt, TRUE);
    cc_xml_filter_choice choice = doc->filtercb(doc->filtercbdata, doc, elt, FALSE);
    switch (choice) {
    case KEEP:
      break;
    case DISCARD:
      {
        if (doc->parsestack.getLength() > 0) {
          cc_xml_elt * parent = doc->parsestack[doc->parsestack.getLength()-1];
          cc_xml_elt_remove_child_x(parent, elt);
          cc_xml_elt_delete_x(elt);
          elt = NULL;
        }
      }
      break;
    default:
      assert(!"invalid filter choice returned from client code");
      break;
    }
  }

#ifdef DEV_DEBUG
  fprintf(stdout, "\nCDATA: '%s'\n", buffer.get());
#endif // DEV_DEBUG
}

void
cc_xml_doc_expat_processing_instruction_handler_cb(void * userdata, const XML_Char * target, const XML_Char * pidata)
{
  XML_Parser parser = static_cast<XML_Parser>(userdata);
  cc_xml_doc * doc = static_cast<cc_xml_doc *>(XML_GetUserData(parser));
#ifdef DEV_DEBUG
  fprintf(stdout, "received processing Instruction...\n");
#endif // DEV_DEBUG
}


void
cc_xml_doc_create_parser_x(cc_xml_doc * doc)
{
  assert(doc && !doc->parser);
  doc->parser = XML_ParserCreate(NULL);
  assert(doc->parser);
  XML_UseParserAsHandlerArg(doc->parser);
  XML_SetUserData(doc->parser, doc);
  XML_SetElementHandler(doc->parser,
                        cc_xml_doc_expat_element_start_handler_cb,
                        cc_xml_doc_expat_element_end_handler_cb);
  XML_SetCharacterDataHandler(doc->parser,
                              cc_xml_doc_expat_character_data_handler_cb);
  XML_SetProcessingInstructionHandler(doc->parser, cc_xml_doc_expat_processing_instruction_handler_cb);
}

void
cc_xml_doc_delete_parser_x(cc_xml_doc * doc)
{
  assert(doc && doc->parser);
  XML_ParserFree(doc->parser);
  doc->parser = NULL;
}

} // anonymous namespace

// *************************************************************************

/*!
  \fn cc_xml_doc * cc_xml_doc_new(void)

  Creates a new cc_xml_doc object that is totally blank.

  \ingroup XML
  \relates cc_xml_doc
*/

cc_xml_doc *
cc_xml_doc_new(void)
{
  cc_xml_doc * doc = new cc_xml_doc;
  assert(doc);
  doc->parser = NULL;
  doc->xmlversion = NULL;
  doc->xmlencoding = NULL;
  doc->filtercb = NULL;
  doc->filtercbdata = NULL;
  doc->filename = NULL;
  doc->root = NULL;
  doc->current = NULL;
  return doc;
}

/*!
  \fn void cc_xml_doc_delete_x(cc_xml_doc * doc)

  Frees up a cc_xml_doc object and all its resources.

  \ingroup XML
  \relates cc_xml_doc
*/

void
cc_xml_doc_delete_x(cc_xml_doc * doc)
{
  assert(doc);
  if (doc->parser) { cc_xml_doc_delete_parser_x(doc); }
  if (doc->xmlversion) delete [] doc->xmlversion;
  if (doc->xmlencoding) delete [] doc->xmlencoding;
  if (doc->filename) delete [] doc->filename;
  if (doc->root) cc_xml_elt_delete_x(doc->root);
  delete doc;
}

/* *************************************************************************

/*!
  \fn void cc_xml_doc_set_filter_cb_x(cc_xml_doc * doc, cc_xml_filter_cb * cb, void * userdata)

  Sets the filter callback for document parsing.  This makes it
  possible to use the parser as a streaming parser, by making the
  parser discard all elements it has read in.

  Elements can only be discarded as they are popped - on push they will be
  kept regardless of what the filter callback returns.

  \ingroup XML
  \relates cc_xml_doc
*/

void
cc_xml_doc_set_filter_cb_x(cc_xml_doc * doc, cc_xml_filter_cb * cb, void * userdata)
{
  doc->filtercb = cb;
  doc->filtercbdata = userdata;
}

/*!
  \fn void cc_xml_doc_get_filter_cb(const cc_xml_doc * doc, cc_xml_filter_cb *& cb, void *& userdata)

  Returns the set filter callback in the \a cb arg and \a userdata arg.

  \ingroup XML
  \relates cc_xml_doc
*/

void
cc_xml_doc_get_filter_cb(const cc_xml_doc * doc, cc_xml_filter_cb *& cb, void *& userdata)
{
  cb = doc->filtercb;
  userdata = doc->filtercbdata;
}

// *************************************************************************

/*!
  Creates an cc_xml_doc and reads a file into it.
  This is just a convenience function.
*/
cc_xml_doc *
cc_xml_read_file(const char * path) // parser.h convenience function
{
  cc_xml_doc * doc = cc_xml_doc_new();
  assert(doc);
  if (!cc_xml_doc_read_file_x(doc, path)) {
    cc_xml_doc_delete_x(doc);
    return NULL;
  }
  return doc;
}

/*!
  Reads a file into the cc_xml_doc object.

  Deletes any old XML DOM the doc contains.
*/

SbBool
cc_xml_doc_read_file_x(cc_xml_doc * doc, const char * path)
{
  assert(doc);
  if (doc->root) {
    cc_xml_elt_delete_x(doc->root);
    doc->root = NULL;
  }

  if (!doc->parser) {
    cc_xml_doc_create_parser_x(doc);
  }

  FILE * fp = fopen(path, "rb");
  if (!fp) {
    // FIXME: error condition
    cc_xml_doc_delete_parser_x(doc);
    return FALSE;
  }

  // read in file in 8K chunks, buffers kept by expat

  SbBool error = FALSE;
  SbBool final = FALSE;

  while (!final && !error) {
    void * buf = XML_GetBuffer(doc->parser, 8192);
    assert(buf);
    int bytes = static_cast<int>(fread(buf, 1, 8192, fp));
    final = feof(fp);
    XML_Status status = XML_ParseBuffer(doc->parser, bytes, final);
    if (status != 0) { cc_xml_doc_handle_parse_error(doc); }
  }

  fclose(fp);

  cc_xml_doc_set_filename_x(doc, path);

  return !error;
}

cc_xml_doc *
cc_xml_read_buffer(const char * buffer) // parser.h convenience function
{
  cc_xml_doc * doc = cc_xml_doc_new();
  assert(doc);
  assert(buffer);
  size_t buflen = strlen(buffer);
  if (!cc_xml_doc_read_buffer_x(doc, buffer, buflen)) {
    cc_xml_doc_delete_x(doc);
    return NULL;
  }
  cc_xml_doc_set_filename_x(doc, "<memory buffer>");
  return doc;
}

namespace {
void cc_xml_doc_parse_buffer_partial_init_x(cc_xml_doc * doc);
}

SbBool
cc_xml_doc_read_buffer_x(cc_xml_doc * doc, const char * buffer, size_t buflen)
{
#ifdef DEV_DEBUG
  fprintf(stdout, "cc_xml_doc_read_buffer_x(%p, %d, %p)\n", doc, (int) buflen, buffer);
#endif // DEV_DEBUG
  cc_xml_doc_parse_buffer_partial_init_x(doc);
  return cc_xml_doc_parse_buffer_partial_done_x(doc, buffer, buflen);
}

// xml_doc_parse_ vs xml_doc_read_

namespace {
void
cc_xml_doc_parse_buffer_partial_init_x(cc_xml_doc * doc) // maybe expose and require explicit?
{
#ifdef DEV_DEBUG
  fprintf(stdout, "cc_xml_doc_parse_buffer_partial_init_x()\n");
#endif // DEV_DEBUG
  assert(doc->parser == NULL);
  cc_xml_doc_create_parser_x(doc);
}
}

SbBool
cc_xml_doc_parse_buffer_partial_x(cc_xml_doc * doc, const char * buffer, size_t buflen)
{
  assert(doc);
#ifdef DEV_DEBUG
  fprintf(stdout, "cc_xml_doc_parse_buffer_partial_x()\n");
#endif // DEV_DEBUG
  if (!doc->parser) {
    cc_xml_doc_parse_buffer_partial_init_x(doc);
  }

  XML_Status ok = XML_Parse(doc->parser, buffer, static_cast<int>(buflen), FALSE);
  if (!ok) { cc_xml_doc_handle_parse_error(doc); }

  return TRUE;
}

SbBool
cc_xml_doc_parse_buffer_partial_done_x(cc_xml_doc * doc, const char * buffer, size_t buflen)
{
#ifdef DEV_DEBUG
  fprintf(stdout, "cc_xml_doc_parse_buffer_partial_done_x()\n");
#endif // DEV_DEBUG
  assert(doc);
  if (!doc->parser) {
    cc_xml_doc_parse_buffer_partial_init_x(doc);
  }
  XML_Status ok = XML_Parse(doc->parser, buffer, static_cast<int>(buflen), TRUE);
  if (!ok) { cc_xml_doc_handle_parse_error(doc); }

  cc_xml_doc_delete_parser_x(doc);
  return TRUE;
}

// *************************************************************************

/*!
  Sets the filename attribute.  Frees old filename data if any.
*/

void
cc_xml_doc_set_filename_x(cc_xml_doc * doc, const char * path)
{
  assert(doc);
  if (doc->filename) {
    delete [] doc->filename;
    doc->filename = NULL;
  }
  doc->filename = cc_xml_strdup(path);
}

/*!
  Returns the filename attribute.  If nothing has been set, NULL is returned.
  If document was read from memory, "<memory buffer>" was returned.
*/

const char *
cc_xml_doc_get_filename(const cc_xml_doc * doc)
{
  assert(doc && doc->filename);
  return doc->filename;
}

/*!
  Sets the current pointer.  Not in use for anything at the moment, and might
  get deprecated.
*/
void
cc_xml_doc_set_current_x(cc_xml_doc * doc, cc_xml_elt * elt)
{
  assert(doc);
  doc->current = elt;
}

/*!
  Returns the current pointer.  Might get deprecated.
*/

cc_xml_elt *
cc_xml_doc_get_current(const cc_xml_doc * doc)
{
  assert(doc);
  return doc->current;
}

/*!
  Sets the root element for the document.  Only useful when
  constructing documents to be written.
*/

void
cc_xml_doc_set_root_x(cc_xml_doc * doc, cc_xml_elt * root)
{
  assert(doc);
  doc->root = root;
}

/*!
  Returns the root element of the document.
*/

cc_xml_elt *
cc_xml_doc_get_root(const cc_xml_doc * doc)
{
  assert(doc);
  return doc->root;
}

// *************************************************************************

void
cc_xml_doc_strip_whitespace_x(cc_xml_doc * doc)
{
  assert(doc);
  return;
#if 0 // FIXME
  cc_xml_path * path = cc_xml_path_new();
  cc_xml_path_set_x(path, CC_XML_CDATA_TYPE, NULL);
  cc_xml_elt * elt = cc_xml_doc_find_element(doc, path);
  while ( elt != NULL ) {
    cc_xml_elt * next = cc_xml_doc_find_next_element(doc, elt, path);
    if ( sc_whitespace_p(cc_xml_elt_get_data(elt)) )  {
      cc_xml_elt_remove_child_x(cc_xml_elt_get_parent(elt), elt);
      cc_xml_elt_delete_x(elt);
    } else {
      cc_xml_elt_strip_whitespace_x(elt);
    }
    elt = next;
  }
  cc_xml_path_delete_x(path);
#endif
}

// *************************************************************************

const cc_xml_elt *
cc_xml_doc_find_element(const cc_xml_doc * doc, cc_xml_path * path)
{
  assert(doc && path);
  return cc_xml_elt_find(doc->root, path);
} // cc_xml_doc_find_element()

const cc_xml_elt *
cc_xml_doc_find_next_element(const cc_xml_doc * doc, cc_xml_elt * prev, cc_xml_path * path)
{
  assert(doc && prev && path);
  return cc_xml_elt_find_next(doc->root, prev, path);
} // cc_xml_doc_find_next_element()

cc_xml_elt *
cc_xml_doc_create_element_x(cc_xml_doc * doc, cc_xml_path * path)
{
  assert(doc && path);
  cc_xml_elt * root = cc_xml_doc_get_root(doc);
  assert(root);
  return cc_xml_elt_create_x(root, path);
} // cc_xml_doc_create_element_x()

// *************************************************************************

SbBool
cc_xml_doc_write_to_buffer(const cc_xml_doc * doc, char *& buffer, size_t & bytes)
{
  assert(doc);
  bytes = static_cast<int>(cc_xml_doc_calculate_size(doc));
  buffer = new char [ bytes + 1 ];

  size_t bytesleft = bytes;
  char * hereptr = buffer;

// macro to advance buffer pointer and decrement bytesleft count
#define ADVANCE_NUM_BYTES(len)          \
  do { const int length = (len);        \
       hereptr += length;               \
       bytesleft -= length; } while (0)

// macro to copy in a string literal and advance pointers
#define ADVANCE_STRING_LITERAL(str)                \
  do { static const char strobj[] = str;           \
       const int strlength = (sizeof(strobj) - 1); \
       strncpy(hereptr, strobj, strlength);        \
       ADVANCE_NUM_BYTES(strlength); } while (0)

// macro to copy in a run-time string and advance pointers
#define ADVANCE_STRING(str)                      \
  do { const int strlength = strlen(str);        \
       strncpy(hereptr, str, strlength);         \
       ADVANCE_NUM_BYTES(strlength); } while (0)

  // duplicate block, see cc_xml_doc_calculate_size()
  ADVANCE_STRING_LITERAL("<?xml version=\"");
  if (doc->xmlversion) {
    ADVANCE_STRING(doc->xmlversion);
  } else {
    ADVANCE_STRING_LITERAL("1.0");
  }
  ADVANCE_STRING_LITERAL("\" encoding=\"");
  if (doc->xmlencoding) {
    ADVANCE_STRING(doc->xmlencoding);
  } else {
    ADVANCE_STRING_LITERAL("UTF-8");
  }
  ADVANCE_STRING_LITERAL("\"?>\n");

  if (doc->root) {
    ADVANCE_NUM_BYTES(cc_xml_elt_write_to_buffer(doc->root, hereptr, bytesleft, 0, 2));
  }

#undef ADVANCE_STRING
#undef ADVANCE_STRING_LITERAL
#undef ADVANCE_NUM_BYTES

  buffer[bytes] = '\0';

  return TRUE;
}

SbBool
cc_xml_doc_write_to_file(cc_xml_doc * doc, const char * path)
{
  assert(doc);
  assert(path);;

  size_t bufsize = 0;
  boost::scoped_array<char> buffer;
  {
    char * bufptr = NULL;
    if (!cc_xml_doc_write_to_buffer(doc, bufptr, bufsize)) {
      return FALSE;
    }
    assert(bufptr);
    buffer.reset(bufptr);
  }

  const size_t bytes = strlen(buffer.get());
  assert(bufsize == bytes);
  FILE * fp = NULL;
  if (strcmp(path, "-") == 0)
    fp = stdout;
  else
    fp = fopen(path, "wb");
  assert(fp != NULL);
  fwrite(buffer.get(), 1, bufsize, fp);
  if (strcmp(path, "-") != 0)
    fclose(fp);

  return TRUE;
}

// *************************************************************************

/*!
  Compare document DOM against other document DOM, and return path to first
  difference.  Returns NULL if documents are equal.  To be used mostly for
  testing the XML I/O code.
*/

cc_xml_path *
cc_xml_doc_diff(const cc_xml_doc * doc, const cc_xml_doc * other)
{
#ifdef DEV_DEBUG
  COIN_STUB();
#endif // DEV_DEBUG
  // FIXME: implement
  return NULL;
}

// In documentp.h
size_t
cc_xml_doc_calculate_size(const cc_xml_doc * doc)
{
  size_t bytes = 0;

// macro to advance a given number of bytes
#define ADVANCE_NUM_BYTES(num) \
  do { bytes += (num); } while (0)

// macro to increment bytecount for string literal
#define ADVANCE_STRING_LITERAL(str) \
  do { static const char strobj[] = str; bytes += (sizeof(strobj) - 1); } while (0)

// macro to increment bytecount for run-time string
#define ADVANCE_STRING(str) \
  do { bytes += strlen(str); } while (0)

  // duplicate block, see cc_xml_doc_write_to_buffer()
  ADVANCE_STRING_LITERAL("<?xml version=\"");
  if (doc->xmlversion) {
    ADVANCE_STRING(doc->xmlversion);
  } else {
    ADVANCE_STRING_LITERAL("1.0");
  }
  ADVANCE_STRING_LITERAL("\" encoding=\"");
  if (doc->xmlencoding) {
    ADVANCE_STRING(doc->xmlencoding);
  } else {
    ADVANCE_STRING_LITERAL("UTF-8");
  }
  ADVANCE_STRING_LITERAL("\"?>\n");

  if (doc->root) {
    ADVANCE_NUM_BYTES(cc_xml_elt_calculate_size(doc->root, 0, 2));
  }

#undef ADVANCE_STRING
#undef ADVANCE_STRING_LITERAL
#undef ADVANCE_NUM_BYTES

  return bytes;
}

/*
  Internal function for centralizing the error message generation.
*/

void
cc_xml_doc_handle_parse_error(const cc_xml_doc * doc)
{
  assert(doc);
  assert(doc->parser);

  const int line = XML_GetCurrentLineNumber(doc->parser);
  const int column = XML_GetCurrentColumnNumber(doc->parser);
  const long byte = XML_GetCurrentByteIndex(doc->parser);

  const char * errormsg = XML_ErrorString(XML_GetErrorCode(doc->parser));

  SbString errorstr;
  errorstr.sprintf("XML parse error, line %d, column %d: %s\n", line, column, errormsg);
  fprintf(stderr, "%s", errorstr.getString());
}

/*
*/

void
cc_xml_doc_handle_parse_warning(const cc_xml_doc * doc, const char * message)
{
  assert(doc);
  assert(doc->parser);
  assert(message);

  const int line = XML_GetCurrentLineNumber(doc->parser);
  const int column = XML_GetCurrentColumnNumber(doc->parser);
  const long byte = XML_GetCurrentByteIndex(doc->parser);

  SbString errorstr;
  errorstr.sprintf("XML parse warning, line %d, column %d: %s\n", line, column, message);
  fprintf(stderr, "%s", errorstr.getString());
}

// *************************************************************************

#ifdef COIN_TEST_SUITE

#include <boost/scoped_array.hpp>
#include <Inventor/C/XML/parser.h>
#include <Inventor/C/XML/path.h>

BOOST_AUTO_TEST_CASE(bufread)
{
  const char * buffer =
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n\n"
"<test value=\"one\" compact=\"\">\n"
"  <b>hei</b>\n"
"</test>\n";
  cc_xml_doc * doc1 = cc_xml_read_buffer(buffer);
  BOOST_CHECK_MESSAGE(doc1 != NULL, "cc_xml_doc_read_buffer() failed");

  fprintf(stdout, "\ndocument read in ok\n");
  boost::scoped_array<char> buffer2;
  size_t bytecount = 0;
  {
    char * bufptr = NULL;
    cc_xml_doc_write_to_buffer(doc1, bufptr, bytecount);
    buffer2.reset(bufptr);
  }

  fprintf(stderr, "OUTPUT:\n'%s'\n", buffer2.get());

  cc_xml_doc * doc2 = cc_xml_read_buffer(buffer2.get());

  cc_xml_path * diffpath = cc_xml_doc_diff(doc1, doc2);
  BOOST_CHECK_MESSAGE(diffpath == NULL, "document read->write->read DOM differences");

  cc_xml_doc_delete_x(doc1);
  cc_xml_doc_delete_x(doc2);
}

#endif // !COIN_TEST_SUITE