#ifndef KARPRABINHASH
#define KARPRABINHASH


#include "characterhash.h"

class KarpRabinHash {

  public:
    KarpRabinHash(int myn, int mywordsize=19) :  hashvalue(0),n(myn), 
    wordsize(mywordsize), 
      hasher( ( 1 << wordsize ) - 1),
      HASHMASK( (0x1<<wordsize)-1),BtoN(1) {
      for (int i=0; i < n ; ++i) {
	      BtoN *= B;
	      BtoN &= HASHMASK;  
      }
    }
    
    template<class container>
    hashvaluetype  hash(container & c) {
    	assert(c.size()==static_cast<uint>(n));
    	hashvaluetype answer(0);
    	for(uint k = 0; k<c.size();++k) {
    		hashvaluetype x(1);
    		for(uint j = 0; j< c.size()-1-k;++j) {
    		  x= (x * B) & HASHMASK;
    		}
    		x= (x * hasher.hashvalues[c[k]]) & HASHMASK;
    		answer=(answer+x) & HASHMASK;
    	}
    	return answer;
    }
    
    void eat(chartype inchar) {
    	hashvalue = (B*hashvalue +  hasher.hashvalues[inchar] )& HASHMASK;
    }
    
    inline void update(chartype outchar, chartype inchar) {
    	hashvalue = (B*hashvalue +  hasher.hashvalues[inchar] - BtoN *  hasher.hashvalues[outchar]) & HASHMASK; 
    } 
       
    hashvaluetype hashvalue;
    const int n, wordsize;
    CharacterHash hasher;
    const hashvaluetype HASHMASK;
    hashvaluetype BtoN;
    static const hashvaluetype B=37;


};


#endif


