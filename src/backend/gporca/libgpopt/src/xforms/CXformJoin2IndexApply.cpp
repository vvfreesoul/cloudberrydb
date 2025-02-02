//---------------------------------------------------------------------------
//	Cloudberry Database
//	Copyright (C) 2013 VMware, Inc. or its affiliates.
//
//	@filename:
//		CXformJoin2IndexApply.cpp
//
//	@doc:
//		Implementation of Inner/Outer Join to Apply transform
//---------------------------------------------------------------------------

#include "gpopt/xforms/CXformJoin2IndexApply.h"

#include "gpos/base.h"

#include "gpopt/base/COptCtxt.h"
#include "gpopt/base/CUtils.h"
#include "gpopt/operators/CLogicalApply.h"
#include "gpopt/operators/CLogicalCTEAnchor.h"
#include "gpopt/operators/CLogicalDynamicGet.h"
#include "gpopt/operators/CLogicalIndexApply.h"
#include "gpopt/operators/CLogicalJoin.h"
#include "gpopt/operators/CLogicalSelect.h"
#include "gpopt/operators/CLogicalUnionAll.h"
#include "gpopt/operators/CNormalizer.h"
#include "gpopt/operators/CPredicateUtils.h"
#include "gpopt/xforms/CSubqueryHandler.h"
#include "gpopt/xforms/CXformUtils.h"
#include "naucrates/md/IMDIndex.h"

using namespace gpmd;
using namespace gpopt;

//---------------------------------------------------------------------------
//	@function:
//		CXformJoin2IndexApply::Exfp
//
//	@doc:
//		Compute xform promise for a given expression handle;
//
//---------------------------------------------------------------------------
CXform::EXformPromise
CXformJoin2IndexApply::Exfp(CExpressionHandle &exprhdl) const
{
	if (0 == exprhdl.DeriveUsedColumns(2)->Size() ||
		exprhdl.DeriveHasSubquery(2) || exprhdl.HasOuterRefs())
	{
		return CXform::ExfpNone;
	}

	return CXform::ExfpHigh;
}

//---------------------------------------------------------------------------
//	@function:
//		CXformJoin2IndexApply::ComputeColumnSets
//
//	@doc:
//		Based on the inner and the scalar expression, it computes scalar expression
//		columns, outer references and required columns.
//		Caller does not take ownership of ppcrsScalarExpr.
//		Caller takes ownership of ppcrsOuterRefs and ppcrsReqd.
//
//---------------------------------------------------------------------------
void
CXformJoin2IndexApply::ComputeColumnSets(CMemoryPool *mp,
										 CExpression *pexprInner,
										 CExpression *pexprScalar,
										 CColRefSet **ppcrsScalarExpr,
										 CColRefSet **ppcrsOuterRefs,
										 CColRefSet **ppcrsReqd)
{
	CColRefSet *pcrsInnerOutput = pexprInner->DeriveOutputColumns();
	*ppcrsScalarExpr = pexprScalar->DeriveUsedColumns();
	*ppcrsOuterRefs = GPOS_NEW(mp) CColRefSet(mp, **ppcrsScalarExpr);
	(*ppcrsOuterRefs)->Difference(pcrsInnerOutput);

	*ppcrsReqd = GPOS_NEW(mp) CColRefSet(mp);
	(*ppcrsReqd)->Include(pcrsInnerOutput);
	(*ppcrsReqd)->Include(*ppcrsScalarExpr);
	(*ppcrsReqd)->Difference(*ppcrsOuterRefs);
}

//---------------------------------------------------------------------------
//	@function:
//		CXformJoin2IndexApply::CreateFullIndexApplyAlternatives
//
//	@doc:
//		Helper to add IndexApply expression to given xform results container
//
//---------------------------------------------------------------------------
void
CXformJoin2IndexApply::CreateHomogeneousIndexApplyAlternatives(
	CMemoryPool *mp, COperator *joinOp, CExpression *pexprOuter,
	CExpression *pexprInner, CExpression *pexprScalar,
	CExpression *origJoinPred, CExpression *nodesToInsertAboveIndexGet,
	CExpression *endOfNodesToInsertAboveIndexGet,
	CTableDescriptor *ptabdescInner, CXformResult *pxfres,
	IMDIndex::EmdindexType emdtype) const
{
	GPOS_ASSERT(nullptr != pexprOuter);
	GPOS_ASSERT(nullptr != pexprInner);
	GPOS_ASSERT(nullptr != pexprScalar);
	GPOS_ASSERT(nullptr != ptabdescInner);
	GPOS_ASSERT(nullptr != pxfres);
	GPOS_ASSERT(IMDIndex::EmdindBtree == emdtype ||
				IMDIndex::EmdindBitmap == emdtype);

	const ULONG ulIndices = ptabdescInner->IndexCount();
	if (0 == ulIndices)
	{
		return;
	}

	// derive the scalar and relational properties to build set of required columns
	CColRefSet *pcrsScalarExpr = nullptr;
	CColRefSet *outer_refs = nullptr;
	CColRefSet *pcrsReqd = nullptr;
	ComputeColumnSets(mp, pexprInner, pexprScalar, &pcrsScalarExpr, &outer_refs,
					  &pcrsReqd);

	if (IMDIndex::EmdindBtree == emdtype)
	{
		CreateHomogeneousBtreeIndexApplyAlternatives(
			mp, joinOp, pexprOuter, pexprInner, pexprScalar, origJoinPred,
			nodesToInsertAboveIndexGet, endOfNodesToInsertAboveIndexGet,
			ptabdescInner, pcrsScalarExpr, outer_refs, pcrsReqd, ulIndices,
			pxfres);
	}
	else
	{
		CreateHomogeneousBitmapIndexApplyAlternatives(
			mp, joinOp, pexprOuter, pexprInner, pexprScalar, origJoinPred,
			nodesToInsertAboveIndexGet, endOfNodesToInsertAboveIndexGet,
			ptabdescInner, outer_refs, pcrsReqd, pxfres);
	}

	//clean-up
	pcrsReqd->Release();
	outer_refs->Release();
}

//---------------------------------------------------------------------------
//	@function:
//		CXformJoin2IndexApply::CreateHomogeneousBtreeIndexApplyAlternatives
//
//	@doc:
//		Helper to add IndexApply expression to given xform results container
//		for homogeneous b-tree indexes
//
//---------------------------------------------------------------------------
void
CXformJoin2IndexApply::CreateHomogeneousBtreeIndexApplyAlternatives(
	CMemoryPool *mp, COperator *joinOp, CExpression *pexprOuter,
	CExpression *pexprInner, CExpression *pexprScalar,
	CExpression *origJoinPred, CExpression *nodesToInsertAboveIndexGet,
	CExpression *endOfNodesToInsertAboveIndexGet,
	CTableDescriptor *ptabdescInner, CColRefSet *pcrsScalarExpr,
	CColRefSet *outer_refs, CColRefSet *pcrsReqd, ULONG ulIndices,
	CXformResult *pxfres)
{
	// array of expressions in the scalar expression
	CExpressionArray *pdrgpexpr =
		CPredicateUtils::PdrgpexprConjuncts(mp, pexprScalar);
	GPOS_ASSERT(pdrgpexpr->Size() > 0);

	// find the indexes whose included columns meet the required columns
	CMDAccessor *md_accessor = COptCtxt::PoctxtFromTLS()->Pmda();
	const IMDRelation *pmdrel = md_accessor->RetrieveRel(ptabdescInner->MDId());

	for (ULONG ul = 0; ul < ulIndices; ul++)
	{
		IMDId *pmdidIndex = pmdrel->IndexMDidAt(ul);
		const IMDIndex *pmdindex = md_accessor->RetrieveIndex(pmdidIndex);

		CreateAlternativesForBtreeIndex(
			mp, joinOp, pexprOuter, pexprInner, origJoinPred,
			nodesToInsertAboveIndexGet, endOfNodesToInsertAboveIndexGet,
			md_accessor, pdrgpexpr, pcrsScalarExpr, outer_refs, pcrsReqd,
			pmdrel, pmdindex, pxfres);
	}

	//clean-up
	pdrgpexpr->Release();
}

//---------------------------------------------------------------------------
//	@function:
//		CXformJoin2IndexApply::CreateAlternativesForBtreeIndex
//
//	@doc:
//		Helper to add IndexApply expression to given xform results container
//		for homogeneous b-tree indexes.
//
//---------------------------------------------------------------------------
void
CXformJoin2IndexApply::CreateAlternativesForBtreeIndex(
	CMemoryPool *mp, COperator *joinOp, CExpression *pexprOuter,
	CExpression *pexprInner, CExpression *origJoinPred,
	CExpression *nodesToInsertAboveIndexGet,
	CExpression *endOfNodesToInsertAboveIndexGet, CMDAccessor *md_accessor,
	CExpressionArray *pdrgpexprConjuncts, CColRefSet *pcrsScalarExpr,
	CColRefSet *outer_refs, CColRefSet *pcrsReqd, const IMDRelation *pmdrel,
	const IMDIndex *pmdindex, CXformResult *pxfres)
{
	CExpression *pexprLogicalIndexGet = CXformUtils::PexprLogicalIndexGet(
		mp, md_accessor, pexprInner, joinOp->UlOpId(), pdrgpexprConjuncts,
		pcrsReqd, pcrsScalarExpr, outer_refs, pmdindex, pmdrel);
	if (nullptr != pexprLogicalIndexGet)
	{
		// second child has residual predicates, create an apply of outer and inner
		// and add it to xform results
		CColRefArray *colref_array = outer_refs->Pdrgpcr(mp);
		CExpression *indexGetWithOptionalSelect = pexprLogicalIndexGet;

		if (COperator::EopLogicalDynamicGet == pexprInner->Pop()->Eopid())
		{
			indexGetWithOptionalSelect =
				CXformUtils::PexprRedundantSelectForDynamicIndex(
					mp, pexprLogicalIndexGet);
			pexprLogicalIndexGet->Release();
		}

		CExpression *rightChildOfApply =
			CXformUtils::AddALinearStackOfUnaryExpressions(
				mp, indexGetWithOptionalSelect, nodesToInsertAboveIndexGet,
				endOfNodesToInsertAboveIndexGet);
		BOOL isOuterJoin = false;

		switch (joinOp->Eopid())
		{
			case COperator::EopLogicalInnerJoin:
				isOuterJoin = false;
				break;

			case COperator::EopLogicalLeftOuterJoin:
				isOuterJoin = true;
				break;

			default:
				// this type of join operator is not supported
				return;
		}

		pexprOuter->AddRef();
		CExpression *pexprIndexApply = GPOS_NEW(mp) CExpression(
			mp,
			GPOS_NEW(mp)
				CLogicalIndexApply(mp, colref_array, isOuterJoin, origJoinPred),
			pexprOuter, rightChildOfApply,
			CPredicateUtils::PexprConjunction(mp, nullptr /*pdrgpexpr*/));
		pxfres->Add(pexprIndexApply);
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CXformJoin2IndexApply::CreateHomogeneousBitmapIndexApplyAlternatives
//
//	@doc:
//		Helper to add IndexApply expression to given xform results container
//		for homogeneous bitmap indexes.
//
//---------------------------------------------------------------------------
void
CXformJoin2IndexApply::CreateHomogeneousBitmapIndexApplyAlternatives(
	CMemoryPool *mp, COperator *joinOp, CExpression *pexprOuter,
	CExpression *pexprInner, CExpression *pexprScalar,
	CExpression *origJoinPred, CExpression *nodesToInsertAboveIndexGet,
	CExpression *endOfNodesToInsertAboveIndexGet,
	CTableDescriptor *ptabdescInner, CColRefSet *outer_refs,
	CColRefSet *pcrsReqd, CXformResult *pxfres)
{
	CLogical *popGet = CLogical::PopConvert(pexprInner->Pop());
	CExpression *pexprLogicalIndexGet = CXformUtils::PexprBitmapTableGet(
		mp, popGet, joinOp->UlOpId(), ptabdescInner, pexprScalar, outer_refs,
		pcrsReqd);
	if (nullptr != pexprLogicalIndexGet)
	{
		// second child has residual predicates, create an apply of outer and inner
		// and add it to xform results
		CColRefArray *colref_array = outer_refs->Pdrgpcr(mp);
		CExpression *indexGetWithOptionalSelect = pexprLogicalIndexGet;

		if (COperator::EopLogicalDynamicGet == popGet->Eopid())
		{
			indexGetWithOptionalSelect =
				CXformUtils::PexprRedundantSelectForDynamicIndex(
					mp, pexprLogicalIndexGet);
			pexprLogicalIndexGet->Release();
		}

		CExpression *rightChildOfApply =
			CXformUtils::AddALinearStackOfUnaryExpressions(
				mp, indexGetWithOptionalSelect, nodesToInsertAboveIndexGet,
				endOfNodesToInsertAboveIndexGet);
		BOOL isOuterJoin = false;

		switch (joinOp->Eopid())
		{
			case COperator::EopLogicalInnerJoin:
				isOuterJoin = false;
				break;

			case COperator::EopLogicalLeftOuterJoin:
				isOuterJoin = true;
				break;

			default:
				// this type of join operator is not supported
				return;
		}

		pexprOuter->AddRef();
		CExpression *pexprIndexApply = GPOS_NEW(mp) CExpression(
			mp,
			GPOS_NEW(mp)
				CLogicalIndexApply(mp, colref_array, isOuterJoin, origJoinPred),
			pexprOuter, rightChildOfApply,
			CPredicateUtils::PexprConjunction(mp, nullptr /*pdrgpexpr*/));
		pxfres->Add(pexprIndexApply);
	}
}

// EOF
