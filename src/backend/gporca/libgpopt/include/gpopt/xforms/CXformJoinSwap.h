//---------------------------------------------------------------------------
//	Cloudberry Database
//	Copyright (C) 2012 EMC Corp.
//
//	@filename:
//		CXformJoinSwap.h
//
//	@doc:
//		Join swap transformation
//---------------------------------------------------------------------------
#ifndef GPOPT_CXformJoinSwap_H
#define GPOPT_CXformJoinSwap_H

#include "gpos/base.h"

#include "gpopt/operators/CPatternLeaf.h"
#include "gpopt/xforms/CXformExploration.h"
#include "gpopt/xforms/CXformUtils.h"

namespace gpopt
{
using namespace gpos;

//---------------------------------------------------------------------------
//	@class:
//		CXformJoinSwap
//
//	@doc:
//		Join swap transformation
//
//---------------------------------------------------------------------------
template <class TJoinTop, class TJoinBottom>
class CXformJoinSwap : public CXformExploration
{
private:
public:
	CXformJoinSwap(const CXformJoinSwap &) = delete;

	// ctor
	explicit CXformJoinSwap(CMemoryPool *mp)
		: CXformExploration(
			  // pattern
			  GPOS_NEW(mp) CExpression(
				  mp, GPOS_NEW(mp) TJoinTop(mp),
				  GPOS_NEW(mp) CExpression	// left child is a join tree
				  (mp, GPOS_NEW(mp) TJoinBottom(mp),
				   GPOS_NEW(mp) CExpression(
					   mp, GPOS_NEW(mp) CPatternLeaf(mp)),	// left child
				   GPOS_NEW(mp) CExpression(
					   mp, GPOS_NEW(mp) CPatternLeaf(mp)),	// right child
				   GPOS_NEW(mp) CExpression(
					   mp, GPOS_NEW(mp) CPatternLeaf(mp))  // predicate
				   ),
				  GPOS_NEW(mp) CExpression	// right child is a pattern leaf
				  (mp, GPOS_NEW(mp) CPatternLeaf(mp)),
				  GPOS_NEW(mp) CExpression(
					  mp, GPOS_NEW(mp) CPatternLeaf(mp))  // top-join predicate
				  ))
	{
	}

	// dtor
	~CXformJoinSwap() override = default;

	// compute xform promise for a given expression handle
	EXformPromise
	Exfp(CExpressionHandle &  // exprhdl
	) const override
	{
		return CXform::ExfpHigh;
	}

	// actual transform
	void
	Transform(CXformContext *pxfctxt, CXformResult *pxfres,
			  CExpression *pexpr) const override
	{
		GPOS_ASSERT(nullptr != pxfctxt);
		GPOS_ASSERT(FPromising(pxfctxt->Pmp(), this, pexpr));
		GPOS_ASSERT(FCheckPattern(pexpr));

		CMemoryPool *mp = pxfctxt->Pmp();

		CExpression *pexprResult =
			CXformUtils::PexprSwapJoins(mp, pexpr, (*pexpr)[0]);
		if (nullptr == pexprResult)
		{
			return;
		}

		pxfres->Add(pexprResult);
	}

};	// class CXformJoinSwap

}  // namespace gpopt

#endif	// !GPOPT_CXformJoinSwap_H

// EOF
