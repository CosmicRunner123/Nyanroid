/*	see copyright notice in squirrel.h */
#ifndef _SQSTRING_H_
#define _SQSTRING_H_

inline SQHash _hashstr (const SQChar *s, size_t l)
{
		SQHash h = (SQHash)l;  /* seed */
		size_t step = (l>>5)|1;  /* if string is too long, don't hash all its chars */
		for (; l>=step; l-=step)
			h = h ^ ((h<<5)+(h>>2)+(unsigned short)*(s++));
		return h;
}

/* Nyanroid: the hash a 64-bit build computes (SQHash is size_t there, so the high bits fold back
   in through h>>2), kept to its low 32 bits: a table's slot is the hash masked by its size, so the
   slots and so the tables' order are the 64-bit build's. For the 64-bit Diesel exes. */
inline SQHash _hashstr64 (const SQChar *s, size_t l)
{
		uint64_t h = (uint64_t)l;
		size_t step = (l>>5)|1;
		for (; l>=step; l-=step)
			h = h ^ ((h<<5)+(h>>2)+(unsigned short)*(s++));
		return (SQHash)h;
}

struct SQString : public SQRefCounted
{
	SQString(){}
	~SQString(){}
public:
	static SQString *Create(SQSharedState *ss, const SQChar *, SQInteger len = -1 );
	SQInteger Next(const SQObjectPtr &refpos, SQObjectPtr &outkey, SQObjectPtr &outval);
	void Release();
	SQSharedState *_sharedstate;
	SQString *_next; //chain for the string table
	SQInteger _len;
	SQHash _hash;
	SQChar _val[1];
};



#endif //_SQSTRING_H_
