#ifndef GENERALHASH
#define GENERALHASH

#include <iostream>
#include <vector>

#include "characterhash.h"

using namespace std;

enum{NOPRECOMP,FULLPRECOMP};
template <int precomputationtype=NOPRECOMP>
class GeneralHash {
  public:
    GeneralHash(int myn, int mywordsize = 19): 
          hashvalue(0),
          wordsize(mywordsize),
          n(myn), 
          irreduciblepoly(0), 
          hasher(( 1<<wordsize ) - 1),
          lastbit(static_cast<hashvaluetype>(1)<<wordsize),
           precomputedshift(precomputationtype==FULLPRECOMP ? (1<<n) : 0){
    		  if(wordsize == 19) {
       			 irreduciblepoly = 1 + (1<<2) + (1<<3) + (1<<5) 
       			 + (1<<6) + (1<<7) + (1<<12) + (1<<16) + (1<<17) 
       			 + (1<<18) + (1<<19);
      			} else if (wordsize == 9) {
        			irreduciblepoly = 1+(1<<2)+(1<<3)+(1<<5)+(1<<9);
      			} else {
        			cerr << "unsupport wordsize "<<wordsize << endl;
      			}
      		   // in case the precomp is activated at the template level
      		   if(precomputationtype==FULLPRECOMP) {
      		   	for(hashvaluetype x = 0; x<precomputedshift.size();++x) {
      		   		hashvaluetype leftover = x << (wordsize-n);
      		   		fastleftshift(leftover, n);
      		   		precomputedshift[x]=leftover;
      		   	}
      		   }
    }

    
    inline void fastleftshift(hashvaluetype & x, int r) const {
      for (int i = 0; i < r;++i) {
        x  <<= 1;
        if(( x & lastbit) == lastbit)
          x ^= irreduciblepoly;
      }
    }
    
    inline void fastleftshiftn(hashvaluetype & x) const {
      x=
      // take the last n bits and look-up the result
      precomputedshift[(x >> (wordsize-n))] 
      ^
      // then just shift the first L-n bits
      ((x << n) & (lastbit -1 ));
    }
    
    inline void update(chartype outchar, chartype inchar) {
      hashvalue <<= 1;
      if(( hashvalue & lastbit) == lastbit)
          hashvalue ^= irreduciblepoly;
      //
      hashvaluetype z (hasher.hashvalues[outchar]);
      // the compiler should optimize away the next if/else
      if(precomputationtype==FULLPRECOMP) { 
        fastleftshiftn(z);
        hashvalue ^= z ^ hasher.hashvalues[inchar];
      } else { 
        fastleftshift(z,n);
        hashvalue ^= z ^ hasher.hashvalues[inchar];
      }
    }
    
    
    
    void eat(chartype inchar) {
      fastleftshift(hashvalue,1);
      hashvalue ^=  hasher.hashvalues[inchar];
    }
    

    template<class container>
    hashvaluetype  hash(container & c) const {
    	assert(c.size()==static_cast<uint>(n));
    	hashvaluetype answer(0);
    	for(uint k = 0; k<c.size();++k) {
    		fastleftshift(answer, 1) ;
    		answer ^= hasher.hashvalues[c[k]];
    	}
    	return answer;
    }

    hashvaluetype hashvalue;
    const int wordsize;
    const int n;
    hashvaluetype irreduciblepoly;
    CharacterHash hasher;
    const hashvaluetype lastbit;
    vector<hashvaluetype> precomputedshift;

};

    

#endif

