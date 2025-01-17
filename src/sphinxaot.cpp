//
// $Id$
//

//
// Copyright (c) 2011-2016, Andrew Aksyonoff
// Copyright (c) 2011-2016, Sphinx Technologies Inc
// All rights reserved
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//
// Based on AOT lemmatizer, http://aot.ru/
// Copyright (c) 2004-2014, Alexey Sokirko and others
//

#include "sphinx.h"
#include "sphinxint.h"
#include "sphinxutils.h"

//////////////////////////////////////////////////////////////////////////
// LEMMATIZER
//////////////////////////////////////////////////////////////////////////

const uint8_t	AOT_POS_UNKNOWN				= 0xff;
const int	AOT_MIN_PREDICTION_SUFFIX	= 3;
const uint8_t	AOT_MORPH_ANNOT_CHAR		= '+';
const int	AOT_MAX_ALPHABET_SIZE		= 54;
const uint32_t	AOT_NOFORM					= 0xffffffffUL;
const uint32_t	AOT_ORIGFORM				= 0xfffffffeUL;

static int	g_iCacheSize				= 262144; // in bytes, so 256K


#define		AOT_MODEL_NO(_a)	((_a)>>18)
#define		AOT_ITEM_NO(_a)		(((_a)&0x3FFFF)>>9)
#define		AOT_PREFIX_NO(_a)	((_a)&0x1FF)


/// morpohological form info
struct CMorphForm
{
	uint8_t		m_FlexiaLen;
	uint8_t		m_PrefixLen;
	uint8_t		m_POS;
	uint8_t		m_Dummy;
	char		m_Prefix[4];
	char		m_Flexia[24];
};


/// alphabet descriptor
struct AlphabetDesc_t
{
	int			m_iSize;
	uint8_t		m_dCode2Alpha [ AOT_MAX_ALPHABET_SIZE ];
	uint8_t		m_dCode2AlphaWA [ AOT_MAX_ALPHABET_SIZE ];
};


/// alphabet codec
class CABCEncoder : public ISphNoncopyable
{
public:
	int			m_AlphabetSize;
	int			m_Alphabet2Code[256];
	int			m_Alphabet2CodeWithoutAnnotator[256];

	void		InitAlphabet ( const AlphabetDesc_t & tDesc );
	bool		CheckABCWithoutAnnotator ( const uint8_t * pWord ) const;
	uint32_t		DecodeFromAlphabet ( const uint8_t * sPath, int iPath ) const;
};

/// morphology automaton node, 1:31
/// 1 bit for "final or not" flag
/// 31 bits for index to relations (pointer to the first child)
struct CMorphAutomNode
{
	uint32_t		m_Data;
	uint32_t		GetChildrenStart() const	{ return m_Data&(0x80000000-1); }
	bool		IsFinal() const				{ return (m_Data&0x80000000) > 0; }
};


/// morphology automaton relation, 8:24
/// 8 bits for relational char (aka next char in current form)
/// 24 bites for index to nodes (pointer to the next level node)
struct CMorphAutomRelation
{
	uint32_t		m_Data;
	uint32_t		GetChildNo() const			{ return m_Data & 0xffffff; }
	uint8_t		GetRelationalChar() const	{ return (uint8_t)(m_Data>>24); }
};


/// morphology automaton
class CMorphAutomat : public CABCEncoder
{
protected:
	CMorphAutomNode *		m_pNodes;
	int						m_NodesCount;
	CMorphAutomRelation *	m_pRelations;
	int						m_RelationsCount;

	int						m_iCacheSize;
	CSphTightVector<int>	m_ChildrenCache;

	void	BuildChildrenCache ( int iCacheSize );
	int		FindStringAndPassAnnotChar ( const uint8_t * pText ) const;

public:
	CMorphAutomat ()
		: m_pNodes ( NULL )
		, m_NodesCount ( 0 )
		, m_pRelations ( NULL )
		, m_RelationsCount ( 0 )
		, m_iCacheSize ( 0 )
	{}

	~CMorphAutomat ()
	{
		SafeDelete ( m_pNodes );
		SafeDelete ( m_pRelations );
	}

	int								GetChildrenCount ( int i ) const	{ return m_pNodes[i+1].GetChildrenStart() - m_pNodes[i].GetChildrenStart(); }
	const CMorphAutomRelation *		GetChildren ( int i ) const			{ return m_pRelations + m_pNodes[i].GetChildrenStart(); }
	const CMorphAutomNode			GetNode ( int i ) const				{ return m_pNodes[i]; }

public:
	bool	LoadPak ( CSphReader & rd, int iCacheSize );
	void	GetInnerMorphInfos ( const uint8_t * pText, uint32_t * Infos ) const;
	int		NextNode ( int NodeNo, uint8_t Child ) const;
};


/// prediction data tuple
struct CPredictTuple
{
	uint16_t				m_ItemNo;
	uint32_t				m_LemmaInfoNo;
	uint8_t				m_PartOfSpeechNo;
};


/// flexia model is basically a vector of morphology forms
/// (there is other meta suff like per-model comments but that is now stripped)
typedef CSphVector<CMorphForm> CFlexiaModel;


/// lemmatizer
class CLemmatizer
{
protected:
	static const int			MAX_PREFIX_LEN = 12;
	static const bool			m_bMaximalPrediction = false;
	bool						m_bIsGerman;

	uint8_t						m_UC[256];

	CMorphAutomat				m_FormAutomat;
	CSphVector<uint16_t>			m_LemmaFlexiaModel;		///< lemma id to flexia model id mapping
	CSphVector<uint8_t>			m_NPSs;
	int							m_PrefixLen [ MAX_PREFIX_LEN ];
	CSphVector<uint8_t>			m_PrefixBlob;

	CMorphAutomat				m_SuffixAutomat;
	CSphVector<uint32_t>			m_ModelFreq;

	bool						IsPrefix ( const uint8_t * sPrefix, int iLen ) const;

	uint32_t						PredictPack ( const CPredictTuple & t ) const		{ return ( m_LemmaFlexiaModel [ t.m_LemmaInfoNo ]<<18 ) + ( t.m_ItemNo<<9 ); }
	bool						PredictFind ( const uint8_t * pWord, int iLen, CSphVector<CPredictTuple> & res ) const;
	void						PredictFindRecursive ( int r, uint8_t * sPath, int iPath, CSphVector<CPredictTuple> & Infos ) const;
	void						PredictByDataBase ( const uint8_t * pWord, int iLen, uint32_t * results, bool is_cap ) const;

public:
	explicit CLemmatizer ( bool IsGerman = false )
		: m_bIsGerman ( IsGerman )
	{}
	CSphVector<CFlexiaModel>	m_FlexiaModels;			///< flexia models
	int							m_iLang;					///< my language

	bool						LemmatizeWord ( uint8_t * pWord, uint32_t * results ) const;
	bool						LoadPak ( CSphReader & rd );
};

//////////////////////////////////////////////////////////////////////////

uint32_t CABCEncoder::DecodeFromAlphabet ( const uint8_t * sPath, int iPath ) const
{
	uint32_t c = 1;
	uint32_t Result = 0;
	for ( const uint8_t * sMax = sPath+iPath; sPath<sMax; sPath++ )
	{
		Result += m_Alphabet2CodeWithoutAnnotator[*sPath] * c;
		c *= m_AlphabetSize - 1;
	}
	return Result;
}


bool CABCEncoder::CheckABCWithoutAnnotator ( const uint8_t * pWord ) const
{
	while ( *pWord )
		if ( m_Alphabet2CodeWithoutAnnotator [ *pWord++ ]==-1 )
			return false;
	return true;
}


void CABCEncoder::InitAlphabet ( const AlphabetDesc_t & tDesc )
{
	m_AlphabetSize = tDesc.m_iSize;
	for ( int i=0; i<256; i++ )
	{
		m_Alphabet2Code[i] = -1;
		m_Alphabet2CodeWithoutAnnotator[i] = -1;
	}
	for ( int i=0; i<m_AlphabetSize; i++ )
		m_Alphabet2Code [ tDesc.m_dCode2Alpha[i] ] = i;
	for ( int i=0; i<m_AlphabetSize-1; i++ )
		m_Alphabet2CodeWithoutAnnotator [ tDesc.m_dCode2AlphaWA[i] ] = i;
}

//////////////////////////////////////////////////////////////////////////

void CMorphAutomat::BuildChildrenCache ( int iCacheSize )
{
	iCacheSize /= AOT_MAX_ALPHABET_SIZE*4;
	iCacheSize = Max ( iCacheSize, 0 );
	m_iCacheSize = Min ( m_NodesCount, iCacheSize );

	m_ChildrenCache.Resize ( m_iCacheSize*AOT_MAX_ALPHABET_SIZE );
	m_ChildrenCache.Fill ( -1 );
	for ( int NodeNo=0; NodeNo<m_iCacheSize; NodeNo++ )
	{
		const CMorphAutomRelation * pStart = m_pRelations + m_pNodes [ NodeNo ].GetChildrenStart();
		const CMorphAutomRelation * pEnd = pStart + GetChildrenCount ( NodeNo );
		for ( ; pStart!=pEnd; pStart++ )
		{
			const CMorphAutomRelation & p = *pStart;
			m_ChildrenCache [ NodeNo*AOT_MAX_ALPHABET_SIZE + m_Alphabet2Code [ p.GetRelationalChar() ] ] = p.GetChildNo();
		}
	}
}


bool CMorphAutomat::LoadPak ( CSphReader & rd, int iCacheSize )
{
	rd.Tag ( "automaton-nodes" );
	m_NodesCount = rd.UnzipInt();
	m_pNodes = new CMorphAutomNode [ m_NodesCount+1 ];
	rd.GetBytes ( m_pNodes, m_NodesCount*sizeof(CMorphAutomNode) );

	rd.Tag ( "automaton-relations" );
	m_RelationsCount = rd.UnzipInt();
	m_pRelations = new CMorphAutomRelation [ m_RelationsCount ];
	rd.GetBytes ( m_pRelations, m_RelationsCount*sizeof(CMorphAutomRelation) );

	if ( rd.GetErrorFlag() )
		return false;

	m_pNodes [ m_NodesCount ].m_Data = m_RelationsCount;

#if !USE_LITTLE_ENDIAN
	for ( int i=0; i< m_NodesCount; ++i )
		FlipEndianess ( &m_pNodes[i].m_Data );
	for ( int i=0; i< m_RelationsCount; ++i )
		FlipEndianess ( &m_pRelations[i].m_Data );
#endif

	BuildChildrenCache ( iCacheSize );
	return true;
}


int CMorphAutomat::NextNode ( int NodeNo, uint8_t RelationChar ) const
{
	if ( NodeNo<m_iCacheSize )
	{
		int z = m_Alphabet2Code [ RelationChar ];
		if ( z==-1 )
			return -1;
		return m_ChildrenCache [ NodeNo*AOT_MAX_ALPHABET_SIZE + z ];
	} else
	{
		const CMorphAutomRelation * pStart = m_pRelations + m_pNodes [ NodeNo ].GetChildrenStart();
		const CMorphAutomRelation * pEnd = pStart + GetChildrenCount ( NodeNo );
		for ( ; pStart!=pEnd; pStart++ )
		{
			const CMorphAutomRelation & p = *pStart;
			if ( RelationChar==p.GetRelationalChar() )
				return p.GetChildNo();
		}
		return -1;
	}
}


int	CMorphAutomat::FindStringAndPassAnnotChar ( const uint8_t * pText ) const
{
	int r = 0;
	while ( *pText )
	{
		int nd = NextNode ( r, *pText++ );
		if ( nd==-1 )
			return -1;
		r = nd;
	}
	return NextNode ( r, AOT_MORPH_ANNOT_CHAR ); // passing annotation char
}


void CMorphAutomat::GetInnerMorphInfos ( const uint8_t * pText, uint32_t * Infos ) const
{
	*Infos = AOT_NOFORM;

	int r = FindStringAndPassAnnotChar ( pText );
	if ( r==-1 )
		return;

	// recursively get all interpretations
	const int MAX_DEPTH = 32;
	int iLevel = 0;
	uint8_t sPath[MAX_DEPTH];
	int iChild[MAX_DEPTH];
	int iChildMax[MAX_DEPTH];

	iChild[0] = m_pNodes[r].GetChildrenStart();
	iChildMax[0] = m_pNodes[r+1].GetChildrenStart();

	while ( iLevel>=0 )
	{
		while ( iChild[iLevel]<iChildMax[iLevel] )
		{
			CMorphAutomRelation Rel = m_pRelations[iChild[iLevel]];
			int NodeNo = Rel.GetChildNo();
			sPath[iLevel] = Rel.GetRelationalChar();
			iChild[iLevel]++;
			if ( m_pNodes[NodeNo].IsFinal() )
			{
				*Infos++ = DecodeFromAlphabet ( sPath, iLevel+1 );
			} else
			{
				iLevel++;
				assert ( iLevel<MAX_DEPTH );
				iChild[iLevel] = m_pNodes[NodeNo].GetChildrenStart();
				iChildMax[iLevel] = m_pNodes[NodeNo+1].GetChildrenStart();
			}
		}
		iLevel--;
	}
	*Infos = AOT_NOFORM;
}

//////////////////////////////////////////////////////////////////////////

void CLemmatizer::PredictFindRecursive ( int NodeNo, uint8_t * sPath, int iPath, CSphVector<CPredictTuple> & Infos ) const
{
	const CMorphAutomNode & N = m_SuffixAutomat.GetNode ( NodeNo );
	if ( N.IsFinal() )
	{
		int i = 0;
		while ( i<iPath && sPath[i]!=AOT_MORPH_ANNOT_CHAR )
			i++;

		int j = i+1;
		while ( j<iPath && sPath[j]!=AOT_MORPH_ANNOT_CHAR )
			j++;

		int k = j+1;
		while ( k<iPath && sPath[k]!=AOT_MORPH_ANNOT_CHAR )
			k++;

		CPredictTuple & A = Infos.Add();
		A.m_PartOfSpeechNo = (uint8_t) m_SuffixAutomat.DecodeFromAlphabet ( sPath+i+1, j-i-1 );
		A.m_LemmaInfoNo = m_SuffixAutomat.DecodeFromAlphabet ( sPath+j+1, k-j-1 );
		A.m_ItemNo = (uint16_t) m_SuffixAutomat.DecodeFromAlphabet ( sPath+k+1, iPath-k-1 );
	}

	int Count = m_SuffixAutomat.GetChildrenCount ( NodeNo );
	for ( int i=0; i<Count; i++ )
	{
		const CMorphAutomRelation & p = m_SuffixAutomat.GetChildren ( NodeNo )[i];
		sPath[iPath] = p.GetRelationalChar();
		PredictFindRecursive ( p.GetChildNo(), sPath, iPath+1, Infos );
	}
}


bool CLemmatizer::PredictFind ( const uint8_t * pWord, int iLen, CSphVector<CPredictTuple> & res ) const
{
	// FIXME? we might not want to predict words with annot char inside
	// was: if (ReversedWordForm.find(AnnotChar) != string::npos) return false;

	int r = 0;
	int i = 0;
	const uint8_t * p = pWord + iLen;
	for ( ; i<iLen; i++ )
	{
		int nd = m_SuffixAutomat.NextNode ( r, *--p );
		if ( nd==-1 )
			break;
		r = nd;
	}

	// no prediction by suffix which is less than 3
	if ( i<AOT_MIN_PREDICTION_SUFFIX )
		return false;

	assert ( r!=-1 );
	uint8_t sPath[128];
	PredictFindRecursive ( r, sPath, 0, res );
	return true;
}


bool CLemmatizer::IsPrefix ( const uint8_t * sPrefix, int iLen ) const
{
	// empty prefix is a prefix
	if ( !iLen )
		return true;
	if ( iLen>=MAX_PREFIX_LEN || m_PrefixLen[iLen]<0 )
		return false;

	const uint8_t * p = &m_PrefixBlob [ m_PrefixLen[iLen] ];
	while ( *p==iLen )
	{
		if ( !memcmp ( p+1, sPrefix, iLen ) )
			return true;
		p += 1+iLen;
	}
	return false;
}


/// returns true if matched in dictionary, false if predicted
bool CLemmatizer::LemmatizeWord ( uint8_t * pWord, uint32_t * results ) const
{
	const bool bCap = false; // maybe when we manage to drag this all the way from tokenizer
	const bool bPredict = true;

	// uppercase (and maybe other translations), check, and compute length
	uint8_t * p;
	if ( m_iLang==AOT_RU )
	{
		for ( p = pWord; *p; p++ )
		{
			uint8_t b = m_UC[*p];
			// russian chars are in 0xC0..0xDF range
			// avoid lemmatizing words with other chars in them
			if ( ( b>>5 )!=6 )
			{
				*results = AOT_NOFORM;
				return false;
			}
			// uppercase
			*p = b;
		}
	} else ///< use the alphabet to reduce another letters
	{
		for ( p = pWord; *p; p++ )
		{
			uint8_t b = m_UC[*p];
			// english chars are in 0x61..0x7A range
			// avoid lemmatizing words with other chars in them
			if ( m_FormAutomat.m_Alphabet2CodeWithoutAnnotator[b]<0 )
			{
				*results = AOT_NOFORM;
				return false;
			}
			// uppercase
			*p = b;
		}
	}

	int iLen = (int)( p-pWord );

	// do dictionary lookup
	m_FormAutomat.GetInnerMorphInfos ( pWord, results );
	if ( *results!=AOT_NOFORM )
		return true;
	if_const ( !bPredict )
		return false;

	// attempt prediction by keyword suffix
	// find the longest suffix that finds dictionary results
	// require that suffix to be 4+ chars too
	int iSuffix;
	for ( iSuffix=1; iSuffix<=iLen-4; iSuffix++ )
	{
		m_FormAutomat.GetInnerMorphInfos ( pWord+iSuffix, results );
		if ( *results!=AOT_NOFORM )
			break;
	}

	// cancel suffix predictions with no hyphens, short enough
	// known postfixes, and unknown prefixes
	if ( pWord [ iSuffix-1 ]!='-'
		&& ( iLen-iSuffix )<6
		&& !IsPrefix ( pWord, iSuffix ) )
	{
		*results = AOT_NOFORM;
	}

	// cancel predictions by pronouns, eg [Sem'ykin'ym]
	for ( uint32_t * pRes=results; *pRes!=AOT_NOFORM; pRes++ )
		if ( m_NPSs[ AOT_MODEL_NO ( *pRes ) ]==AOT_POS_UNKNOWN )
	{
		*results = AOT_NOFORM;
		break;
	}

	// what, still no results?
	if ( *results==AOT_NOFORM )
	{
		// attempt prediction by database
		PredictByDataBase ( pWord, iLen, results, bCap );

		// filter out too short flexias
		uint32_t * s = results;
		uint32_t * d = s;
		while ( *s!=AOT_NOFORM )
		{
			const CMorphForm & F = m_FlexiaModels [ AOT_MODEL_NO(*s) ][ AOT_ITEM_NO(*s) ];
			if ( F.m_FlexiaLen<iLen )
				*d++ = *s;
			s++;
		}
		*d = AOT_NOFORM;
	}

	return false;
}


void CLemmatizer::PredictByDataBase ( const uint8_t * pWord, int iLen, uint32_t * FindResults, bool is_cap ) const
{
	// FIXME? handle all-consonant abbreviations anyway?
	// was: if ( CheckAbbreviation ( InputWordStr, FindResults, is_cap ) ) return;

	assert ( *FindResults==AOT_NOFORM );
	uint32_t * pOut = FindResults;
	CSphVector<CPredictTuple> res;

	// if the ABC is wrong this prediction yields too many variants
	if ( m_FormAutomat.CheckABCWithoutAnnotator ( pWord ) )
		PredictFind ( pWord, iLen, res );

	// assume not more than 32 different pos
	int has_nps[32];
	for ( int i=0; i<32; i++ )
		has_nps[i] = -1;

	ARRAY_FOREACH ( j, res )
	{
		uint8_t PartOfSpeechNo = res[j].m_PartOfSpeechNo;
		if_const ( !m_bMaximalPrediction && has_nps[PartOfSpeechNo]!=-1 )
		{
			int iOldFreq = m_ModelFreq [ AOT_MODEL_NO ( FindResults[has_nps[PartOfSpeechNo]] ) ];
			int iNewFreq = m_ModelFreq [ m_LemmaFlexiaModel [ res[j].m_LemmaInfoNo ] ];
			if ( iOldFreq < iNewFreq )
				FindResults [ has_nps [ PartOfSpeechNo ] ] = PredictPack ( res[j] );
			continue;
		}

		has_nps [ PartOfSpeechNo ] = (int)( pOut-FindResults );
		*pOut++ = PredictPack ( res[j] );
		*pOut = AOT_NOFORM;
	}

	if	( has_nps[0]==-1 // no noun
		|| ( is_cap && !m_bIsGerman ) ) // or can be a proper noun (except German, where all nouns are written uppercase)
	{
		static uint8_t CriticalNounLetterPack[4] = "+++";
		PredictFind ( CriticalNounLetterPack, AOT_MIN_PREDICTION_SUFFIX, res );
		*pOut++ = PredictPack ( res.Last() );
		*pOut = AOT_NOFORM;
	}
}


bool CLemmatizer::LoadPak ( CSphReader & rd )
{
	rd.Tag ( "sphinx-aot" );
	int iVer = rd.UnzipInt();
	if ( iVer!=1 )
		return false;

	rd.Tag ( "alphabet-desc" );
	AlphabetDesc_t tDesc;
	tDesc.m_iSize = rd.UnzipInt();
	rd.GetBytes ( tDesc.m_dCode2Alpha, tDesc.m_iSize );
	rd.GetBytes ( tDesc.m_dCode2AlphaWA, tDesc.m_iSize );

	m_FormAutomat.InitAlphabet ( tDesc );
	m_SuffixAutomat.InitAlphabet ( tDesc );

	rd.Tag ( "uc-table" );
	rd.GetBytes ( m_UC, 256 );

	// caching forms can help a lot (from 4% with 256K cache to 13% with 110M cache)
	rd.Tag ( "forms-automaton" );
	m_FormAutomat.LoadPak ( rd, g_iCacheSize );

	rd.Tag ( "flexia-models" );
	m_FlexiaModels.Resize ( rd.UnzipInt() );
	ARRAY_FOREACH ( i, m_FlexiaModels )
	{
		m_FlexiaModels[i].Resize ( rd.UnzipInt() );
		ARRAY_FOREACH ( j, m_FlexiaModels[i] )
		{
			CMorphForm & F = m_FlexiaModels[i][j];
			F.m_FlexiaLen = (uint8_t) rd.GetByte();
			rd.GetBytes ( F.m_Flexia, F.m_FlexiaLen );
			F.m_PrefixLen = (uint8_t) rd.GetByte();
			rd.GetBytes ( F.m_Prefix, F.m_PrefixLen );
			F.m_POS = (uint8_t) rd.GetByte();

			assert ( F.m_FlexiaLen<sizeof(F.m_Flexia) );
			assert ( F.m_PrefixLen<sizeof(F.m_Prefix) );
			F.m_Flexia[F.m_FlexiaLen] = 0;
			F.m_Prefix[F.m_PrefixLen] = 0;
		}
	}

	rd.Tag ( "prefixes" );
	for ( int i=0; i<MAX_PREFIX_LEN; i++ )
		m_PrefixLen[i] = rd.UnzipInt();
	m_PrefixBlob.Resize ( rd.UnzipInt() );
	rd.GetBytes ( m_PrefixBlob.Begin(), m_PrefixBlob.GetLength() );

	rd.Tag ( "lemma-flexia-models" );
	m_LemmaFlexiaModel.Resize ( rd.UnzipInt() );
	ARRAY_FOREACH ( i, m_LemmaFlexiaModel )
		m_LemmaFlexiaModel[i] = (uint16_t) rd.UnzipInt();

	// build model freqs
	m_ModelFreq.Resize ( m_FlexiaModels.GetLength() );
	m_ModelFreq.Fill ( 0 );
	ARRAY_FOREACH ( i, m_LemmaFlexiaModel )
		m_ModelFreq [ m_LemmaFlexiaModel[i] ]++;

	rd.Tag ( "nps-vector" );
	m_NPSs.Resize ( rd.UnzipInt() );
	rd.GetBytes ( m_NPSs.Begin(), m_NPSs.GetLength() );

	// caching predictions does not measurably affect performance though
	rd.Tag ( "prediction-automaton" );
	m_SuffixAutomat.LoadPak ( rd, 0 );

	rd.Tag ( "eof" );
	return !rd.GetErrorFlag();
}

//////////////////////////////////////////////////////////////////////////
// SPHINX MORPHOLOGY INTERFACE
//////////////////////////////////////////////////////////////////////////

const char* AOT_LANGUAGES[AOT_LENGTH] = {"ru", "en", "de" };

static CLemmatizer *	g_pLemmatizers[AOT_LENGTH] = {0};
static CSphNamedInt		g_tDictinfos[AOT_LENGTH];

void sphAotSetCacheSize ( int iCacheSize )
{
	g_iCacheSize = Max ( iCacheSize, 0 );
}

bool AotInit ( const CSphString & sDictFile, CSphString & sError, int iLang )
{
	if ( g_pLemmatizers[iLang] )
		return true;

	CSphAutofile rdFile;
	if ( rdFile.Open ( sDictFile, SPH_O_READ, sError )<0 )
		return false;

	g_pLemmatizers[iLang] = new CLemmatizer ( iLang==AOT_DE );
	g_pLemmatizers[iLang]->m_iLang = iLang;

	CSphReader rd;
	rd.SetFile ( rdFile );
	if ( !g_pLemmatizers[iLang]->LoadPak(rd) )
	{
		sError.SetSprintf ( "failed to load lemmatizer dictionary: %s", rd.GetErrorMessage().cstr() );
		SafeDelete ( g_pLemmatizers[iLang] );
		return false;
	}

	// track dictionary crc
	uint32_t uCrc;
	if ( !sphCalcFileCRC32 ( sDictFile.cstr(), uCrc ) )
	{
		sError.SetSprintf ( "failed to crc32 lemmatizer dictionary %s", sDictFile.cstr() );
		SafeDelete ( g_pLemmatizers[iLang] );
		return false;
	}

	// extract basename
	const char * a = sDictFile.cstr();
	const char * b = a + strlen(a) - 1;
	while ( b>a && b[-1]!='/' && b[-1]!='\\' )
		b--;

	g_tDictinfos[iLang].m_sName = b;
	g_tDictinfos[iLang].m_iValue = (int)uCrc;
	return true;
}

bool sphAotInit ( const CSphString & sDictFile, CSphString & sError, int iLang )
{
	return AotInit ( sDictFile, sError, iLang );
}

static inline bool IsAlpha1251 ( uint8_t c )
{
	return ( c>=0xC0 || c==0xA8 || c==0xB8 );
}

static inline bool IsGermanAlpha1252 ( uint8_t c )
{
	if ( c==0xb5 || c==0xdf )
		return true;

	uint8_t lc = c | 0x20;
	switch ( lc )
	{
	case 0xe2:
	case 0xe4:
	case 0xe7:
	case 0xe8:
	case 0xe9:
	case 0xea:
	case 0xf1:
	case 0xf4:
	case 0xf6:
	case 0xfb:
	case 0xfc:
		return true;
	default:
		return ( lc>0x60 && lc<0x7b );
	}
}

static inline bool IsAlphaAscii ( uint8_t c )
{
	uint8_t lc = c | 0x20;
	return ( lc>0x60 && lc<0x7b );
}

enum EMMITERS {EMIT_1BYTE, EMIT_UTF8RU, EMIT_UTF8};
template < EMMITERS >
inline uint8_t * Emit ( uint8_t * sOut, uint8_t uChar )
{
	if ( uChar=='-' )
		return sOut;
	*sOut++ = uChar | 0x20;
	return sOut;
}

template<>
inline uint8_t * Emit<EMIT_UTF8RU> ( uint8_t * sOut, uint8_t uChar )
{
	if ( uChar=='-' )
		return sOut;
	assert ( uChar!=0xA8 && uChar!=0xB8 ); // no country for yo
	uChar |= 0x20; // lowercase, E0..FF range now
	if ( uChar & 0x10 )
	{
		// F0..FF -> D1 80..D1 8F
		*sOut++ = 0xD1;
		*sOut++ = uChar - 0x70;
	} else
	{
		// E0..EF -> D0 B0..D0 BF
		*sOut++ = 0xD0;
		*sOut++ = uChar - 0x30;
	}
	return sOut;
}

template<>
inline uint8_t * Emit<EMIT_UTF8> ( uint8_t * sOut, uint8_t uChar )
{
	if ( uChar=='-' )
		return sOut;

	if ( uChar!=0xDF ) // don't touch 'ss' umlaut
		uChar |= 0x20;

	if ( uChar & 0x80 )
	{
		*sOut++ = 0xC0 | (uChar>>6);
		*sOut++ = 0x80 | (uChar&0x3F); // NOLINT
	} else
		*sOut++ = uChar;
	return sOut;
}

template < EMMITERS IS_UTF8 >
inline void CreateLemma ( uint8_t * sOut, const uint8_t * sBase, int iBaseLen, bool bFound, const CFlexiaModel & M, const CMorphForm & F )
{
	// cut the form prefix
	int PrefixLen = F.m_PrefixLen;
	if	( bFound || strncmp ( (const char*)sBase, F.m_Prefix, PrefixLen )==0 )
	{
		sBase += PrefixLen;
		iBaseLen -= PrefixLen;
	}

	// FIXME! maybe handle these lemma wide prefixes too?
#if 0
	const string & LemmPrefix = m_pParent->m_Prefixes[m_InnerAnnot.m_PrefixNo];
	if ( m_bFound
		|| (
		( m_InputWordBase.substr ( 0, LemmPrefix.length() )==LemmPrefix ) &&
		( m_InputWordBase.substr ( LemmPrefix.length(), F.m_PrefixStr.length() )==F.m_PrefixStr ) ) )
	{
		m_InputWordBase.erase ( 0, LemmPrefix.length()+ M.m_PrefixStr.length() );
		m_bPrefixesWereCut = true;
	}
#endif

	// cut the form suffix and append the lemma suffix
	// UNLESS this was a predicted form, and form suffix does not fully match!
	// eg. word=GUBARIEVICHA, flexion=IEIVICHA, so this is not really a matching lemma
	int iSuff = F.m_FlexiaLen;
	if ( bFound || ( iBaseLen>=iSuff && strncmp ( (const char*)sBase+iBaseLen-iSuff, F.m_Flexia, iSuff )==0 ) )
	{
		// ok, found and/or suffix matches, the usual route
		int iCodePoints = 0;
		iBaseLen -= iSuff;
		while ( iBaseLen-- && iCodePoints<SPH_MAX_WORD_LEN )
		{
			sOut = Emit<IS_UTF8> ( sOut, *sBase++ );
			iCodePoints++;
		}

		int iLemmaSuff = M[0].m_FlexiaLen;
		const char * sFlexia = M[0].m_Flexia;
		while ( iLemmaSuff-- && iCodePoints<SPH_MAX_WORD_LEN ) // OPTIMIZE? can remove len here
		{
			sOut = Emit<IS_UTF8> ( sOut, *sFlexia++ );
			iCodePoints++;
		}
	} else
	{
		// whoops, no suffix match, just copy and lowercase the current base
		while ( iBaseLen-- )
			sOut = Emit<IS_UTF8> ( sOut, *sBase++ );
	}
	*sOut = '\0';
}

static inline bool IsRuFreq2 ( uint8_t * pWord )
{
	if ( pWord[2]!=0 )
		return false;

	int iCode = ( ( pWord[0]<<8 ) + pWord[1] ) | 0x2020;
	switch ( iCode )
	{
		case 0xEDE0: // na
		case 0xEFEE: // po
		case 0xEDE5: // ne
		case 0xEEF2: // ot
		case 0xE7E0: // za
		case 0xEEE1: // ob
		case 0xE4EE: // do
		case 0xF1EE: // so
		case 0xE8E7: // iz
		case 0xE8F5: // ih
		case 0xF8F2: // sht
		case 0xF3EB: // ul
			return true;
	}
	return false;
}

static inline bool IsEnFreq2 ( uint8_t * )
{
	// stub
	return false;
}

static inline bool IsDeFreq2 ( uint8_t * )
{
	// stub
	return false;
}

static inline bool IsRuFreq3 ( uint8_t * pWord )
{
	if ( pWord[3]!=0 )
		return false;
	int iCode = ( ( pWord[0]<<16 ) + ( pWord[1]<<8 ) + pWord[2] ) | 0x202020;
	return ( iCode==0xE8EBE8 || iCode==0xE4EBFF || iCode==0xEFF0E8 // ili, dlya, pri
		|| iCode==0xE3EEE4 || iCode==0xF7F2EE || iCode==0xE1E5E7 ); // god, chto, bez
}

static inline bool IsEnFreq3 ( uint8_t * )
{
	// stub
	return false;
}

static inline bool IsDeFreq3 ( uint8_t * )
{
	// stub
	return false;
}

void sphAotLemmatizeRu1251 ( uint8_t * pWord )
{
	// i must be initialized
	assert ( g_pLemmatizers[AOT_RU] );

	// pass-through 1-char words, and non-Russian words
	if ( !IsAlpha1251(*pWord) || !pWord[1] )
		return;

	// handle a few most frequent 2-char, 3-char pass-through words
	if ( IsRuFreq2(pWord) || IsRuFreq3(pWord) )
		return;

	// do lemmatizing
	// input keyword moves into sForm; LemmatizeWord() will also case fold sForm
	// we will generate results using sForm into pWord; so we need this extra copy
	uint8_t sForm [ SPH_MAX_WORD_LEN*3+4 ]; // aka MAX_KEYWORD_BYTES
	int iFormLen = 0;

	// faster than strlen and strcpy..
	for ( uint8_t * p=pWord; *p; )
		sForm[iFormLen++] = *p++;
	sForm[iFormLen] = '\0';

	uint32_t FindResults[12]; // max results is like 6
	bool bFound = g_pLemmatizers[AOT_RU]->LemmatizeWord ( (uint8_t*)sForm, FindResults );
	if ( FindResults[0]==AOT_NOFORM )
		return;

	// pick a single form
	// picks a noun, if possible, and otherwise prefers shorter forms
	bool bNoun = false;
	for ( int i=0; FindResults[i]!=AOT_NOFORM; i++ )
	{
		const CFlexiaModel & M = g_pLemmatizers[AOT_RU]->m_FlexiaModels [ AOT_MODEL_NO ( FindResults[i] ) ];
		const CMorphForm & F = M [ AOT_ITEM_NO ( FindResults[i] ) ];

		bool bNewNoun = ( F.m_POS==0 );
		if ( i==0 || ( !bNoun && bNewNoun ) )
		{
			CreateLemma<EMIT_1BYTE> ( pWord, sForm, iFormLen, bFound, M, F );
			bNoun = bNewNoun;
		} else if ( bNoun==bNewNoun )
		{
			uint8_t sBuf[256];
			CreateLemma<EMIT_1BYTE> ( sBuf, sForm, iFormLen, bFound, M, F );
			if ( strcmp ( (char*)sBuf, (char*)pWord )<0 )
				strcpy ( (char*)pWord, (char*)sBuf ); // NOLINT
		}
	}
}

void sphAotLemmatize ( uint8_t * pWord, int iLang )
{
	// i must be initialized
	assert ( g_pLemmatizers[iLang] );

	// pass-through 1-char words, and non-Russian words
	if ( !IsAlphaAscii(*pWord) || !pWord[1] )
		return;

	// handle a few most frequent 2-char, 3-char pass-through words
	if ( iLang==AOT_EN && ( IsEnFreq2(pWord) || IsEnFreq3(pWord) ) )
		return;

	if ( iLang==AOT_DE && ( IsDeFreq2(pWord) || IsDeFreq3(pWord) ) )
		return;

	// do lemmatizing
	// input keyword moves into sForm; LemmatizeWord() will also case fold sForm
	// we will generate results using sForm into pWord; so we need this extra copy
	uint8_t sForm [ SPH_MAX_WORD_LEN*3+4 ]; // aka MAX_KEYWORD_BYTES
	int iFormLen = 0;

	// faster than strlen and strcpy..
	for ( uint8_t * p=pWord; *p; )
		sForm[iFormLen++] = *p++;
	sForm[iFormLen] = '\0';

	// do nothing with one-char words
	if ( iFormLen<=1 )
		return;

	uint32_t FindResults[12]; // max results is like 6
	bool bFound = g_pLemmatizers[iLang]->LemmatizeWord ( (uint8_t*)sForm, FindResults );
	if ( FindResults[0]==AOT_NOFORM )
		return;

	// pick a single form
	// picks a noun, if possible, and otherwise prefers shorter forms
	bool bNoun = false;
	for ( int i=0; FindResults[i]!=AOT_NOFORM; i++ )
	{
		const CFlexiaModel & M = g_pLemmatizers[iLang]->m_FlexiaModels [ AOT_MODEL_NO ( FindResults[i] ) ];
		const CMorphForm & F = M [ AOT_ITEM_NO ( FindResults[i] ) ];

		bool bNewNoun = ( F.m_POS==0 );
		if ( i==0 || ( !bNoun && bNewNoun ) )
		{
			CreateLemma<EMIT_1BYTE> ( pWord, sForm, iFormLen, bFound, M, F );
			bNoun = bNewNoun;
		} else if ( bNoun==bNewNoun )
		{
			uint8_t sBuf[256];
			CreateLemma<EMIT_1BYTE> ( sBuf, sForm, iFormLen, bFound, M, F );
			if ( strcmp ( (char*)sBuf, (char*)pWord )<0 )
				strcpy ( (char*)pWord, (char*)sBuf ); // NOLINT
		}
	}
}

static inline bool IsRussianAlphaUtf8 ( const uint8_t * pWord )
{
	// letters, windows-1251, utf-8
	// A..YA, C0..DF, D0 90..D0 AF
	// a..p, E0..EF, D0 B0..D0 BF
	// r..ya, F0..FF, D1 80..D1 8F
	// YO, A8, D0 81
	// yo, B8, D1 91
	if ( pWord[0]==0xD0 )
		if ( pWord[1]==0x81 || ( pWord[1]>=0x90 && pWord[1]<0xC0 ) )
			return true;
	if ( pWord[0]==0xD1 )
		if ( pWord[1]>=0x80 && pWord[1]<=0x91 && pWord[1]!=0x90 )
			return true;
	return false;
}

void sphAotLemmatizeDe1252 ( uint8_t * pWord )
{
	// i must be initialized
	assert ( g_pLemmatizers[AOT_DE] );

	// pass-through 1-char words, and non-German words
	if ( !IsGermanAlpha1252(*pWord) || !pWord[1] )
		return;

	// handle a few most frequent 2-char, 3-char pass-through words
	if ( IsDeFreq2(pWord) || IsDeFreq3(pWord) )
		return;

	// do lemmatizing
	// input keyword moves into sForm; LemmatizeWord() will also case fold sForm
	// we will generate results using sForm into pWord; so we need this extra copy
	uint8_t sForm [ SPH_MAX_WORD_LEN*3+4 ]; // aka MAX_KEYWORD_BYTES
	int iFormLen = 0;

	// faster than strlen and strcpy..
	for ( uint8_t * p=pWord; *p; )
		sForm[iFormLen++] = *p++;
	sForm[iFormLen] = '\0';

	uint32_t FindResults[12]; // max results is like 6
	bool bFound = g_pLemmatizers[AOT_DE]->LemmatizeWord ( (uint8_t*)sForm, FindResults );
	if ( FindResults[0]==AOT_NOFORM )
		return;

	// pick a single form
	// picks a noun, if possible, and otherwise prefers shorter forms
	bool bNoun = false;
	for ( int i=0; FindResults[i]!=AOT_NOFORM; i++ )
	{
		const CFlexiaModel & M = g_pLemmatizers[AOT_DE]->m_FlexiaModels [ AOT_MODEL_NO ( FindResults[i] ) ];
		const CMorphForm & F = M [ AOT_ITEM_NO ( FindResults[i] ) ];

		bool bNewNoun = ( F.m_POS==0 );
		if ( i==0 || ( !bNoun && bNewNoun ) )
		{
			CreateLemma<EMIT_1BYTE> ( pWord, sForm, iFormLen, bFound, M, F );
			bNoun = bNewNoun;
		} else if ( bNoun==bNewNoun )
		{
			uint8_t sBuf[256];
			CreateLemma<EMIT_1BYTE> ( sBuf, sForm, iFormLen, bFound, M, F );
			if ( strcmp ( (char*)sBuf, (char*)pWord )<0 )
				strcpy ( (char*)pWord, (char*)sBuf ); // NOLINT
		}
	}
}

/// returns length in bytes (aka chars) if all letters were russian and converted
/// returns 0 and aborts early if non-russian letters are encountered
static inline int Utf8ToWin1251 ( uint8_t * pOut, const uint8_t * pWord )
{
	// YO, win A8, utf D0 81
	// A..YA, win C0..DF, utf D0 90..D0 AF
	// a..p, win E0..EF, utf D0 B0..D0 BF
	// r..ya, win F0..FF, utf D1 80..D1 8F
	// yo, win B8, utf D1 91
	static const uint8_t dTable[128] =
	{
		0, 0xa8, 0, 0, 0, 0, 0, 0, // 00
		0, 0, 0, 0, 0, 0, 0, 0, // 08
		0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, // 10
		0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf, // 18
		0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, // 20
		0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf, // 28
		0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, // 30
		0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef, // 38
		0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, // 40
		0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff, // 48
		0, 0xb8, 0, 0, 0, 0, 0, 0, // 50
		0, 0, 0, 0, 0, 0, 0, 0, // 58
		0, 0, 0, 0, 0, 0, 0, 0, // 60
		0, 0, 0, 0, 0, 0, 0, 0, // 68
		0, 0, 0, 0, 0, 0, 0, 0, // 70
		0, 0, 0, 0, 0, 0, 0, 0 // 78
	};

	uint8_t * pStart = pOut;
	while ( *pWord )
	{
		// russian utf-8 letters begin with either D0 or D1
		// and any valid 2nd utf-8 byte must be in 80..BF range
		if ( ( *pWord & 0xFE )!=0xD0 )
			return 0;
		assert ( pWord[1]>=0x80 && pWord[1]<0xC0 );

		// table index D0 80..BF to 0..3F, and D1 80..BF to 40..7F
		uint8_t uWin = dTable [ ( pWord[1] & 0x7F ) + ( ( pWord[0] & 1 )<<6 ) ];
		pWord += 2;

		if ( !uWin )
			return 0;
		*pOut++ = uWin;
	}

	*pOut = '\0';
	return (int)( pOut-pStart );
}

/// returns length in bytes (aka chars) if all letters were converted
/// returns 0 and aborts early if non-western letters are encountered
static inline int Utf8ToWin1252 ( uint8_t * pOut, const uint8_t * pWord )
{
	uint8_t * pStart = pOut;
	while ( *pWord )
	{
		if ( (*pWord)&0x80 )
		{
			if ( ((*pWord)&0xFC)==0xC0 )
			{
				*pOut++ = ( pWord[1] & 0x7F ) + ( ( pWord[0] & 3 )<<6 );
				pWord += 2;
			} else
				return 0;
		} else
			*pOut++ = *pWord++;
	}

	*pOut = '\0';
	return (int)( pOut-pStart );
}

static inline bool IsGermanAlphaUtf8 ( const uint8_t * pWord )
{
	// letters, windows-1252, utf-8
	// A..Z, trivial
	if ( pWord[0]>0x40 && pWord[0]<0x5b )
		return true;

	// a..z, also trivial
	if ( pWord[0]>0x60 && pWord[0]<0x7b )
		return true;

	// mu, 0xb5
	if ( pWord[0]==0xC2 && pWord[1]==0xB5 )
		return true;

	// some upper
	if ( pWord[0]==0xC3 )
	{
		if ( pWord[1]==0X9F ) // ss umlaut
			return true;
		switch ( pWord[1] | 0x20 )
		{
			case 0xA2: // umlauts
			case 0xA4:
			case 0xA7:
			case 0xA8:
			case 0xA9:
			case 0xAa:
			case 0xB1:
			case 0xB4:
			case 0xB6:
			case 0xBb:
			case 0xBc:
				return true;
		}
	}
	return false;
}

static inline void Win1251ToLowercaseUtf8 ( uint8_t * pOut, const uint8_t * pWord )
{
	while ( *pWord )
	{
		// a..p, E0..EF maps to D0 B0..D0 BF
		// r..ya, F0..FF maps to D1 80..D1 8F
		// yo maps to D1 91
		if ( *pWord>=0xC0 )
		{
			uint8_t iCh = ( *pWord | 0x20 ); // lowercase
			uint8_t iF = ( iCh>>4 ) & 1; // 0xE? or 0xF? value
			*pOut++ = 0xD0 + iF;
			*pOut++ = iCh - 0x30 - ( iF<<6 );
		} else if ( *pWord==0xA8 || *pWord==0xB8 )
		{
			*pOut++ = 0xD1;
			*pOut++ = 0x91;
		} else
			assert ( false );
		pWord++;
	}
	*pOut++ = '\0';
}

static inline void Win1252ToLowercaseUtf8 ( uint8_t * pOut, const uint8_t * pWord )
{
	while ( *pWord )
	{
		if ( !((*pWord)&0x80) )
			*pOut++ = *pWord | 0x20;
		else
		{
			*pOut++ = 0xC0 | ((*pWord)>>6);
			*pOut++ = 0x80 | ((*pWord)&0x3F);
		}
		++pWord;
	}
	*pOut++ = '\0';
}

void sphAotLemmatizeRuUTF8 ( uint8_t * pWord )
{
	// i must be initialized
	assert ( g_pLemmatizers[AOT_RU] );

	// only if the word is russian
	if ( !IsRussianAlphaUtf8(pWord) )
		return;

	// convert to Windows-1251
	// failure means we should not lemmatize this
	uint8_t sBuf [ SPH_MAX_WORD_LEN+4 ];
	if ( !Utf8ToWin1251 ( sBuf, pWord ) )
		return;

	// lemmatize, convert back, done!
	sphAotLemmatizeRu1251 ( sBuf );
	Win1251ToLowercaseUtf8 ( pWord, sBuf );
}

void sphAotLemmatizeDeUTF8 ( uint8_t * pWord )
{
	// i must be initialized
	assert ( g_pLemmatizers[AOT_DE] );

	// only if the word is german
	if ( !IsGermanAlphaUtf8(pWord) )
		return;

	// convert to Windows-1252
	// failure means we should not lemmatize this
	uint8_t sBuf [ SPH_MAX_WORD_LEN+4 ];
	if ( !Utf8ToWin1252 ( sBuf, pWord ) )
		return;

	// lemmatize, convert back, done!
	sphAotLemmatizeDe1252 ( sBuf );
	Win1252ToLowercaseUtf8 ( pWord, sBuf );
}

void sphAotLemmatizeRu ( CSphVector<CSphString> & dLemmas, const uint8_t * pWord )
{
	assert ( g_pLemmatizers[AOT_RU] );
	if ( !IsRussianAlphaUtf8(pWord) )
		return;

	uint8_t sForm [ SPH_MAX_WORD_LEN+4 ];
	int iFormLen = 0;
	iFormLen = Utf8ToWin1251 ( sForm, pWord );

	if ( iFormLen<2 || IsRuFreq2(sForm) )
		return;
	if ( iFormLen<3 || IsRuFreq3(sForm) )
		return;

	uint32_t FindResults[12]; // max results is like 6
	bool bFound = g_pLemmatizers[AOT_RU]->LemmatizeWord ( (uint8_t*)sForm, FindResults );
	if ( FindResults[0]==AOT_NOFORM )
		return;

	for ( int i=0; FindResults[i]!=AOT_NOFORM; i++ )
	{
		const CFlexiaModel & M = g_pLemmatizers[AOT_RU]->m_FlexiaModels [ AOT_MODEL_NO ( FindResults[i] ) ];
		const CMorphForm & F = M [ AOT_ITEM_NO ( FindResults[i] ) ];

		uint8_t sRes [ 3*SPH_MAX_WORD_LEN+4 ];

		CreateLemma<EMIT_UTF8RU> ( sRes, sForm, iFormLen, bFound, M, F );
		dLemmas.Add ( (const char*)sRes );
	}

	// OPTIMIZE?
	dLemmas.Uniq();
}

void sphAotLemmatizeDe ( CSphVector<CSphString> & dLemmas, const uint8_t * pWord )
{
	assert ( g_pLemmatizers[AOT_DE] );
	if ( !IsGermanAlphaUtf8(pWord) )
		return;

	uint8_t sForm [ SPH_MAX_WORD_LEN+4 ];
	int iFormLen = 0;
	iFormLen = Utf8ToWin1252 ( sForm, pWord );

	if ( iFormLen<=1 )
		return;

	if ( IsDeFreq2(sForm) || IsDeFreq3(sForm) )
		return;

	uint32_t FindResults[12]; // max results is like 6
	bool bFound = g_pLemmatizers[AOT_DE]->LemmatizeWord ( (uint8_t*)sForm, FindResults );
	if ( FindResults[0]==AOT_NOFORM )
		return;

	for ( int i=0; FindResults[i]!=AOT_NOFORM; i++ )
	{
		const CFlexiaModel & M = g_pLemmatizers[AOT_DE]->m_FlexiaModels [ AOT_MODEL_NO ( FindResults[i] ) ];
		const CMorphForm & F = M [ AOT_ITEM_NO ( FindResults[i] ) ];

		uint8_t sRes [ 3*SPH_MAX_WORD_LEN+4 ];

		CreateLemma<EMIT_UTF8> ( sRes, sForm, iFormLen, bFound, M, F );
		dLemmas.Add ( (const char*)sRes );
	}

	// OPTIMIZE?
	dLemmas.Uniq();
}

// generic lemmatize for other languages
void sphAotLemmatize ( CSphVector<CSphString> & dLemmas, const uint8_t * pWord, int iLang )
{
	assert ( iLang!=AOT_RU ); // must be processed by the specialized function
	assert ( g_pLemmatizers[iLang] );

	if ( !IsAlphaAscii(*pWord) )
		return;

	uint8_t sForm [ SPH_MAX_WORD_LEN+4 ];
	int iFormLen = 0;

	while ( *pWord )
		sForm [ iFormLen++ ] = *pWord++;
	sForm [ iFormLen ] = '\0';

	if ( iFormLen<=1 )
		return;

	if ( iLang==AOT_EN && ( IsEnFreq2(sForm) || IsEnFreq3(sForm) ) )
		return;

	if ( iLang==AOT_DE && ( IsDeFreq2(sForm) || IsDeFreq3(sForm) ) )
		return;

	uint32_t FindResults[12]; // max results is like 6
	bool bFound = g_pLemmatizers[iLang]->LemmatizeWord ( (uint8_t*)sForm, FindResults );
	if ( FindResults[0]==AOT_NOFORM )
		return;

	for ( int i=0; FindResults[i]!=AOT_NOFORM; i++ )
	{
		const CFlexiaModel & M = g_pLemmatizers[iLang]->m_FlexiaModels [ AOT_MODEL_NO ( FindResults[i] ) ];
		const CMorphForm & F = M [ AOT_ITEM_NO ( FindResults[i] ) ];

		uint8_t sRes [ 3*SPH_MAX_WORD_LEN+4 ];
		CreateLemma<EMIT_1BYTE> ( sRes, sForm, iFormLen, bFound, M, F );

		dLemmas.Add ( (const char*)sRes );
	}

	// OPTIMIZE?
	dLemmas.Uniq();
}


const CSphNamedInt & sphAotDictinfo ( int iLang )
{
	return g_tDictinfos[iLang];
}

//////////////////////////////////////////////////////////////////////////

/// token filter for AOT morphology indexing
/// AOT may return multiple (!) morphological hypotheses for a single token
/// we return such additional hypotheses as blended tokens
class CSphAotTokenizerTmpl : public CSphTokenFilter
{
protected:
	uint8_t		m_sForm [ SPH_MAX_WORD_LEN*3+4 ];	///< aka MAX_KEYWORD_BYTES
	int			m_iFormLen;							///< in bytes, but in windows-1251 that is characters, too
	bool		m_bFound;							///< found or predicted?
	uint32_t		m_FindResults[12];					///< max results is like 6
	int			m_iCurrent;							///< index in m_FindResults that was just returned, -1 means no blending
	uint8_t		m_sToken [ SPH_MAX_WORD_LEN*3+4 ];	///< to hold generated lemmas
	uint8_t		m_sOrigToken [ SPH_MAX_WORD_LEN*3+4 ];	///< to hold original token
	bool		m_bIndexExact;

	const CSphWordforms *	m_pWordforms;

public:
	CSphAotTokenizerTmpl ( ISphTokenizer * pTok, CSphDict * pDict, bool bIndexExact, int DEBUGARG(iLang) )
		: CSphTokenFilter ( pTok )
	{
		assert ( pTok );
		assert ( g_pLemmatizers[iLang] );
		m_iCurrent = -1;
		m_FindResults[0] = AOT_NOFORM;
		m_pWordforms = NULL;
		if ( pDict )
		{
			// tricky bit
			// one does not simply take over the wordforms from the dict
			// that would break saving of the (embedded) wordforms data
			// but as this filter replaces wordforms
			m_pWordforms = pDict->GetWordforms();
			pDict->DisableWordforms();
		}
		m_bIndexExact = bIndexExact;
	}

	void SetBuffer ( const uint8_t * sBuffer, int iLength )
	{
		m_pTokenizer->SetBuffer ( sBuffer, iLength );
	}

	bool TokenIsBlended() const
	{
		return m_iCurrent>=0 || m_pTokenizer->TokenIsBlended();
	}

	uint64_t GetSettingsFNV () const
	{
		uint64_t uHash = CSphTokenFilter::GetSettingsFNV();
		uHash ^= (uint64_t)m_pWordforms;
		uint32_t uFlags = m_bIndexExact ? 1 : 0;
		uHash = sphFNV64 ( &uFlags, sizeof(uFlags), uHash );
		return uHash;
	}
};

class CSphAotTokenizerRu : public CSphAotTokenizerTmpl
{
public:
	CSphAotTokenizerRu ( ISphTokenizer * pTok, CSphDict * pDict, bool bIndexExact )
		: CSphAotTokenizerTmpl ( pTok, pDict, bIndexExact, AOT_RU )
	{}

	ISphTokenizer * Clone ( ESphTokenizerClone eMode ) const
	{
		// this token filter must NOT be created as escaped
		// it must only be used during indexing time, NEVER in searching time
		assert ( eMode==SPH_CLONE_INDEX );
		CSphAotTokenizerRu * pClone = new CSphAotTokenizerRu ( m_pTokenizer->Clone ( eMode ), NULL, m_bIndexExact );
		if ( m_pWordforms )
			pClone->m_pWordforms = m_pWordforms;
		return pClone;
	}

	uint8_t * GetToken()
	{
		m_eTokenMorph = SPH_TOKEN_MORPH_RAW;

		// any pending lemmas left?
		if ( m_iCurrent>=0 )
		{
			++m_iCurrent;
			assert ( m_FindResults[m_iCurrent]!=AOT_NOFORM );

			// return original token
			if ( m_FindResults[m_iCurrent]==AOT_ORIGFORM )
			{
				assert ( m_FindResults[m_iCurrent+1]==AOT_NOFORM );
				strncpy ( (char*)m_sToken, (char*)m_sOrigToken, sizeof(m_sToken) );
				m_iCurrent = -1;
				m_eTokenMorph = SPH_TOKEN_MORPH_ORIGINAL;
				return m_sToken;
			}

			// generate that lemma
			const CFlexiaModel & M = g_pLemmatizers[AOT_RU]->m_FlexiaModels [ AOT_MODEL_NO ( m_FindResults [ m_iCurrent ] ) ];
			const CMorphForm & F = M [ AOT_ITEM_NO ( m_FindResults [ m_iCurrent ] ) ];
			CreateLemma<EMIT_UTF8RU> ( m_sToken, m_sForm, m_iFormLen, m_bFound, M, F );

			// is this the last one? gotta tag it non-blended
			if ( m_FindResults [ m_iCurrent+1 ]==AOT_NOFORM )
				m_iCurrent = -1;

			if ( m_pWordforms && m_pWordforms->m_bHavePostMorphNF )
				m_pWordforms->ToNormalForm ( m_sToken, false, false );

			m_eTokenMorph = SPH_TOKEN_MORPH_GUESS;
			return m_sToken;
		}

		// ok, time to work on a next word
		assert ( m_iCurrent<0 );
		uint8_t * pToken = m_pTokenizer->GetToken();
		if ( !pToken )
			return NULL;

		// pass-through blended parts
		if ( m_pTokenizer->TokenIsBlended() )
			return pToken;

		// pass-through matched wordforms
		if ( m_pWordforms && m_pWordforms->ToNormalForm ( pToken, true, false ) )
			return pToken;

		// pass-through 1-char "words"
		if ( pToken[1]=='\0' )
			return pToken;

		// pass-through non-Russian words
		if ( !IsRussianAlphaUtf8 ( pToken ) )
			return pToken;

		// convert or copy regular tokens
		m_iFormLen = Utf8ToWin1251 ( m_sForm, pToken );

		// do nothing with one-char words
		if ( m_iFormLen<=1 )
			return pToken;

		// handle a few most frequent 2-char, 3-char pass-through words
		// OPTIMIZE? move up?
		if ( IsRuFreq2 ( m_sForm ) || IsRuFreq3 ( m_sForm ) )
			return pToken;

		// lemmatize
		m_bFound = g_pLemmatizers[AOT_RU]->LemmatizeWord ( m_sForm, m_FindResults );
		if ( m_FindResults[0]==AOT_NOFORM )
		{
			assert ( m_iCurrent<0 );
			return pToken;
		}

		// schedule original form for return, if needed
		if ( m_bIndexExact )
		{
			int i = 1;
			while ( m_FindResults[i]!=AOT_NOFORM )
				i++;
			m_FindResults[i] = AOT_ORIGFORM;
			m_FindResults[i+1] = AOT_NOFORM;
			strncpy ( (char*)m_sOrigToken, (char*)pToken, sizeof(m_sOrigToken) );
		}

		// in any event, prepare the first lemma for return
		const CFlexiaModel & M = g_pLemmatizers[AOT_RU]->m_FlexiaModels [ AOT_MODEL_NO ( m_FindResults[0] ) ];
		const CMorphForm & F = M [ AOT_ITEM_NO ( m_FindResults[0] ) ];
		CreateLemma<EMIT_UTF8RU> ( pToken, m_sForm, m_iFormLen, m_bFound, M, F );

		// schedule lemmas 2+ for return
		if ( m_FindResults[1]!=AOT_NOFORM )
			m_iCurrent = 0;

		// suddenly, post-morphology wordforms
		if ( m_pWordforms && m_pWordforms->m_bHavePostMorphNF )
			m_pWordforms->ToNormalForm ( pToken, false, false );

		m_eTokenMorph = SPH_TOKEN_MORPH_GUESS;
		return pToken;
	}
};

class CSphAotTokenizer : public CSphAotTokenizerTmpl
{
	AOT_LANGS		m_iLang;
public:
	CSphAotTokenizer ( ISphTokenizer * pTok, CSphDict * pDict, bool bIndexExact, int iLang )
		: CSphAotTokenizerTmpl ( pTok, pDict, bIndexExact, iLang )
		, m_iLang ( AOT_LANGS(iLang) )
	{}

	ISphTokenizer * Clone ( ESphTokenizerClone eMode ) const
	{
		// this token filter must NOT be created as escaped
		// it must only be used during indexing time, NEVER in searching time
		assert ( eMode==SPH_CLONE_INDEX );
		CSphAotTokenizer * pClone = new CSphAotTokenizer ( m_pTokenizer->Clone ( eMode ), NULL, m_bIndexExact, m_iLang );
		if ( m_pWordforms )
			pClone->m_pWordforms = m_pWordforms;
		return pClone;
	}

	uint8_t * GetToken()
	{
		m_eTokenMorph = SPH_TOKEN_MORPH_RAW;

		// any pending lemmas left?
		if ( m_iCurrent>=0 )
		{
			++m_iCurrent;
			assert ( m_FindResults[m_iCurrent]!=AOT_NOFORM );

			// return original token
			if ( m_FindResults[m_iCurrent]==AOT_ORIGFORM )
			{
				assert ( m_FindResults[m_iCurrent+1]==AOT_NOFORM );
				strncpy ( (char*)m_sToken, (char*)m_sOrigToken, sizeof(m_sToken) );
				m_iCurrent = -1;
				m_eTokenMorph = SPH_TOKEN_MORPH_ORIGINAL;
				return m_sToken;
			}

			// generate that lemma
			const CFlexiaModel & M = g_pLemmatizers[m_iLang]->m_FlexiaModels [ AOT_MODEL_NO ( m_FindResults [ m_iCurrent ] ) ];
			const CMorphForm & F = M [ AOT_ITEM_NO ( m_FindResults [ m_iCurrent ] ) ];
			CreateLemma<EMIT_UTF8> ( m_sToken, m_sForm, m_iFormLen, m_bFound, M, F );

			// is this the last one? gotta tag it non-blended
			if ( m_FindResults [ m_iCurrent+1 ]==AOT_NOFORM )
				m_iCurrent = -1;

			if ( m_pWordforms && m_pWordforms->m_bHavePostMorphNF )
				m_pWordforms->ToNormalForm ( m_sToken, false, false );

			m_eTokenMorph = SPH_TOKEN_MORPH_GUESS;
			return m_sToken;
		}

		// ok, time to work on a next word
		assert ( m_iCurrent<0 );
		uint8_t * pToken = m_pTokenizer->GetToken();
		if ( !pToken )
			return NULL;

		// pass-through blended parts
		if ( m_pTokenizer->TokenIsBlended() )
			return pToken;

		// pass-through matched wordforms
		if ( m_pWordforms && m_pWordforms->ToNormalForm ( pToken, true, false ) )
			return pToken;

		// pass-through 1-char "words"
		if ( pToken[1]=='\0' )
			return pToken;

		// pass-through non-Russian words
		if ( m_iLang==AOT_DE )
		{
			if ( !IsGermanAlphaUtf8 ( pToken ) )
				return pToken;
		} else
		{
			if ( !IsGermanAlpha1252 ( pToken[0] ) )
				return pToken;
		}

		// convert or copy regular tokens
		if ( m_iLang==AOT_DE )
			m_iFormLen = Utf8ToWin1252 ( m_sForm, pToken );
		else
		{
			// manual strlen and memcpy; faster this way
			uint8_t * p = pToken;
			m_iFormLen = 0;
			while ( *p )
				m_sForm [ m_iFormLen++ ] = *p++;
			m_sForm [ m_iFormLen ] = '\0';
		}

		// do nothing with one-char words
		if ( m_iFormLen<=1 )
			return pToken;

		// handle a few most frequent 2-char, 3-char pass-through words
		// OPTIMIZE? move up?
		if ( ( m_iLang==AOT_DE && ( IsDeFreq2 ( m_sForm ) || IsDeFreq3 ( m_sForm ) ) )
			|| ( m_iLang==AOT_EN && ( IsEnFreq2 ( m_sForm ) || IsEnFreq3 ( m_sForm ) ) ) )
			return pToken;

		// lemmatize
		m_bFound = g_pLemmatizers[m_iLang]->LemmatizeWord ( m_sForm, m_FindResults );
		if ( m_FindResults[0]==AOT_NOFORM )
		{
			assert ( m_iCurrent<0 );
			return pToken;
		}

		// schedule original form for return, if needed
		if ( m_bIndexExact )
		{
			int i = 1;
			while ( m_FindResults[i]!=AOT_NOFORM )
				i++;
			m_FindResults[i] = AOT_ORIGFORM;
			m_FindResults[i+1] = AOT_NOFORM;
			strncpy ( (char*)m_sOrigToken, (char*)pToken, sizeof(m_sOrigToken) );
		}

		// in any event, prepare the first lemma for return
		const CFlexiaModel & M = g_pLemmatizers[m_iLang]->m_FlexiaModels [ AOT_MODEL_NO ( m_FindResults[0] ) ];
		const CMorphForm & F = M [ AOT_ITEM_NO ( m_FindResults[0] ) ];
		CreateLemma<EMIT_UTF8> ( pToken, m_sForm, m_iFormLen, m_bFound, M, F );

		// schedule lemmas 2+ for return
		if ( m_FindResults[1]!=AOT_NOFORM )
			m_iCurrent = 0;

		// suddenly, post-morphology wordforms
		if ( m_pWordforms && m_pWordforms->m_bHavePostMorphNF )
			m_pWordforms->ToNormalForm ( pToken, false, false );

		m_eTokenMorph = SPH_TOKEN_MORPH_GUESS;
		return pToken;
	}
};


CSphTokenFilter * sphAotCreateFilter ( ISphTokenizer * pTokenizer, CSphDict * pDict, bool bIndexExact, uint32_t uLangMask )
{
	assert ( uLangMask!=0 );
	CSphTokenFilter * pDerivedTokenizer = NULL;
	for ( int i=AOT_BEGIN; i<AOT_LENGTH; ++i )
	{
		if ( uLangMask & (1UL<<i) )
		{
			if ( i==AOT_RU )
				pDerivedTokenizer = new CSphAotTokenizerRu ( pTokenizer, pDict, bIndexExact );
			else
				pDerivedTokenizer = new CSphAotTokenizer ( pTokenizer, pDict, bIndexExact, i );
			pTokenizer = pDerivedTokenizer;
		}
	}
	return pDerivedTokenizer;
}


void sphAotShutdown ()
{
	for ( int i=0; i<AOT_LENGTH; i++ )
		SafeDelete ( g_pLemmatizers[i] );
}

//
// $Id$
//
