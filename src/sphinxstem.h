//
// $Id$
//

//
// Copyright (c) 2001-2016, Andrew Aksyonoff
// Copyright (c) 2008-2016, Sphinx Technologies Inc
// All rights reserved
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

#ifndef _sphinxstem_
#define _sphinxstem_

#include "sphinx.h"

/// initialize English stemmar
void	stem_en_init ();

/// initialize Russian stemmar
void	stem_ru_init ();

/// stem lowercase English word
void	stem_en ( uint8_t * pWord, int iLen );

/// stem lowercase Russian word in Windows-1251 encoding
void	stem_ru_cp1251 ( uint8_t * pWord );

/// stem lowercase Russian word in UTF-8 encoding
void	stem_ru_utf8 ( uint16_t * pWord );

/// initialize Czech stemmer
void	stem_cz_init ();

/// stem lowercase Czech word
void	stem_cz ( uint8_t * pWord );

/// stem Arabic word in UTF-8 encoding
void	stem_ar_utf8 ( uint8_t * word );

/// calculate soundex in-place if the word is lowercase English letters only;
/// do nothing if it's not
void	stem_soundex ( uint8_t * pWord );

/// double metaphone stemmer
void	stem_dmetaphone ( uint8_t * pWord );

/// pre-init AOT setup, cache size (in bytes)
void	sphAotSetCacheSize ( int iCacheSize );

// simple order aot languages
enum AOT_LANGS {AOT_BEGIN=0, AOT_RU=AOT_BEGIN, AOT_EN, AOT_DE, AOT_LENGTH};

// aot lemmatize names
extern const char* AOT_LANGUAGES [AOT_LENGTH];

/// init AOT lemmatizer
bool	sphAotInit ( const CSphString & sDictFile, CSphString & sError, int iLang );

// functions below by design used in indexing time
/// lemmatize (or guess a normal form) a Russian word in Windows-1251 encoding
void	sphAotLemmatizeRu1251 ( uint8_t * pWord );

/// lemmatize (or guess a normal form) a Russian word in UTF-8 encoding, return a single "best" lemma
void	sphAotLemmatizeRuUTF8 ( uint8_t * pWord );

/// lemmatize (or guess a normal form) a German word in Windows-1252 encoding
void	sphAotLemmatizeDe1252 ( uint8_t * pWord );

/// lemmatize (or guess a normal form) a German word in UTF-8 encoding, return a single "best" lemma
void	sphAotLemmatizeDeUTF8 ( uint8_t * pWord );

/// lemmatize (or guess a normal form) a word in single-byte ASCII encoding, return a single "best" lemma
void	sphAotLemmatize ( uint8_t * pWord, int iLang );

// functions below by design used in search time
/// lemmatize (or guess a normal form) a Russian word, return all lemmas
void	sphAotLemmatizeRu ( CSphVector<CSphString> & dLemmas, const uint8_t * pWord );
void	sphAotLemmatizeDe ( CSphVector<CSphString> & dLemmas, const uint8_t * pWord );
void	sphAotLemmatize ( CSphVector<CSphString> & dLemmas, const uint8_t * pWord, int iLang );

/// get lemmatizer dictionary info (file name, crc)
const CSphNamedInt &	sphAotDictinfo ( int iLang );

/// create token filter that returns all morphological hypotheses
/// NOTE, takes over wordforms from pDict, in AOT case they must be handled by the fitler
class CSphTokenFilter;
CSphTokenFilter *		sphAotCreateFilter ( ISphTokenizer * pTokenizer, CSphDict * pDict, bool bIndexExact, uint32_t uLangMask );

/// free lemmatizers on shutdown
void	sphAotShutdown ();

#endif // _sphinxstem_

//
// $Id$
//
