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

#include "sphinx.h"

void stem_soundex ( uint8_t * pWord )
{
	static uint8_t dLetter2Code[27] = "01230120022455012623010202";

	// check if the word only contains lowercase English letters
	uint8_t * p = pWord;
	while ( *p>='a' && *p<='z' )
		p++;
	if ( *p )
		return;

	// do soundex
	p = pWord+1;
	uint8_t * pOut = pWord+1;
	while ( *p )
	{
		uint8_t c = dLetter2Code [ (*p)-'a' ];
		if ( c!='0' && pOut[-1]!=c )
			*pOut++ = c;
		p++;
	}

	while ( pOut-pWord<4 && pOut<p )
		*pOut++ = '0';

	*pOut++ = '\0';
}

//
// $Id$
//
