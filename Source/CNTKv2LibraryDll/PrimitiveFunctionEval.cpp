//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//

// The actual direct forward and backward computation of V2 Functions is containedin here.
// TODO: move most of Dynamite GetValue.cpp here

#include "stdafx.h"
#include "PrimitiveFunction.h"
#include "Utils.h"

#include <vector>
#include <string>

using namespace Microsoft::MSR::CNTK;
using namespace std;

namespace CNTK
{
    // helper to get a slice view, used for both forward and backward of Slice()
    // It returns a TensorView slice (which may have gaps due to strides, e.g. an auto-batched Slice()).
    static NDArrayViewPtr GetSliceView(const NDArrayViewPtr& arg, const Dictionary& attributes, const NDShape& outputShape, bool readOnly, const PrimitiveFunction& funcForErrMsg)
    {
        if (attributes.Size() == 1) // Index() --has no axis or endIndex parameter. and must drop the final axis
        {
            let index = attributes[PrimitiveFunction::AttributeNameBeginIndex].Value<int>();
            let& extent = outputShape.Dimensions(); // note: last dimension is missing; this will strip it in the output
            if (extent.size() + 1 != arg->Shape().Rank())
                LogicError("Variable '%S' Value(): The input and output rank for op Slice when indexing must differ by 1.", funcForErrMsg.AsString().c_str());
            auto startOffset = vector<size_t>(extent.size() + 1, 0);
            startOffset.back() = (size_t) index;
            return arg->Slice(startOffset, extent, vector<size_t>(), NDArrayView::SliceMode::View, readOnly); // slice it
        }
        else
        {
            auto extent = arg->Shape().Dimensions();
            auto startOffset = vector<size_t>(extent.size(), 0);
            if (attributes.Contains(PrimitiveFunction::AttributeNameAxisVec)) // vector of slices
            {
                let& axes         = AsVector<Axis>(attributes[PrimitiveFunction::AttributeNameAxisVec      ].Value<vector<DictionaryValue>>());
                let& beginIndices = AsVector<int> (attributes[PrimitiveFunction::AttributeNameBeginIndexVec].Value<vector<DictionaryValue>>());
                let& endIndices   = AsVector<int> (attributes[PrimitiveFunction::AttributeNameEndIndexVec  ].Value<vector<DictionaryValue>>());
                if (attributes.Contains(PrimitiveFunction::AttributeNameSliceStridesVec))
                    LogicError("Variable '%S' Value(): Strided slicing not yet implemented.", funcForErrMsg.AsString().c_str());
                for (size_t i = 0; i < axes.size(); i++)
                {
                    let axisIndex  = axes[i].StaticAxisIndex();
                    let beginIndex = beginIndices[i];
                    let endIndex   = endIndices[i];
                    startOffset[axisIndex] = beginIndex;
                    extent[axisIndex] = endIndex - beginIndex;
                }
            }
            else // single slice
            {
                let axis       = attributes[PrimitiveFunction::AttributeNameAxis].Value<Axis>();
                let beginIndex = attributes[PrimitiveFunction::AttributeNameBeginIndex].Value<int>();
                let endIndex   = attributes[PrimitiveFunction::AttributeNameEndIndex].Value<int>();
                if (attributes.Contains(PrimitiveFunction::AttributeNameSliceStrides))
                    LogicError("Variable '%S' Value(): Strided slicing not yet implemented.", funcForErrMsg.AsString().c_str());
                let axisIndex = axis.StaticAxisIndex();
                startOffset[axisIndex] = beginIndex;
                extent[axisIndex] = endIndex - beginIndex;
            }
            if (extent != arg->Shape().Dimensions() || any_of(startOffset.begin(), startOffset.end(), [](size_t v) { return v != 0; }))
                return arg->Slice(startOffset, extent, vector<size_t>(), NDArrayView::SliceMode::View, readOnly); // slice it
        }
        return arg;
    }

    // Performs a forward operation.
    // It is assumed that the inputs are immutable, and hence if the result can be expressed as a view (e.g. Reshape()),
    // a view into the an input is returned instead of a newly allocated buffer.
    // For Slice(), a view is returned if the slice is memory-contiguous. Otherwise, a copy is made, so that the
    // result remains compatible with potential subsequent Matrix-library operations.
    // Note: To support auto-batching, this function must only consider attributes when presence of an additional
    // batch axis makes no difference. That is, for example, not the case for ReduceElements over AllStaticAxes().
    // This is addressed as:
    //  - All relative axis arguments must have been normalized already in InferOutputs. Can't call NormalizeStaticAxis() here.
    //  - All reduction axes are already completely specified by the output shape. Can't look at the axis parameter.
    /*static*/ NDArrayViewPtr PrimitiveFunction::ComputeKnowableValue(PrimitiveOpType primitiveOp,  // execute this op
                     const vector<NDArrayViewPtr>& args, const Dictionary& attributes, // on these inputs --TODO: move attributes up
                     const NDShape& outputShape, NDArrayViewPtr&& out, // into this output (if null then create a new one)
                     const PrimitiveFunction& funcForErrMsg)
    {
        // Slice() can either create new data or not, so do it first
        let sliceView = primitiveOp == PrimitiveOpType::Slice ? GetSliceView(args[0], attributes, outputShape, /*readOnly=*/true, funcForErrMsg) : nullptr;

        // first handle ops that do not create new data
        if (primitiveOp == PrimitiveOpType::StopGradient ||
            primitiveOp == PrimitiveOpType::Pass         ||
            primitiveOp == PrimitiveOpType::NoOp         ||
            primitiveOp == PrimitiveOpType::Reshape      ||
            (primitiveOp == PrimitiveOpType::Slice && !out && sliceView->IsContiguous())) // must copy if output buffer provided or data not contiguous
        {
            if (out)
                LogicError("Variable '%S' Value(): An output buffer was passed for op %S that does not need one.", funcForErrMsg.AsString().c_str(), PrimitiveOpTypeName(primitiveOp).c_str());
            let arg = args[0];
            switch (primitiveOp)
            {
            case PrimitiveOpType::Reshape:
                if (arg->Shape() != outputShape)
                     return arg->AsShape(outputShape);
                break;
            case PrimitiveOpType::Slice:
                return sliceView;
            }
            // operation is a no-op: return original argument as is
            //arg->LogToFile(PrimitiveOpTypeName(primitiveOp), stderr);
            return arg;
        }

        // ops that generate output: allocate memory for the result unless memory was passed in
        if (!out)
            out = make_shared<NDArrayView>(args.front()->GetDataType(), outputShape, args.front()->Device());
        else if (out->Shape() != outputShape)
            LogicError("Variable '%S' Value(): The out buffer passed to op %S does not match outputShape.", funcForErrMsg.AsString().c_str(), PrimitiveOpTypeName(primitiveOp).c_str());
        // perform the operation
        auto op          = Microsoft::MSR::CNTK::ElementWiseOperator::opNone;
        auto reductionOp = Microsoft::MSR::CNTK::ElementWiseOperator::opSum;
        double alpha = 1; // changed in ReduceMean() to 1/#elements averaged over
        switch (primitiveOp)
        {
            // binary elementwise ops are done outside, we just set the opcode
        case PrimitiveOpType::Plus:          op = Microsoft::MSR::CNTK::ElementWiseOperator::opSum;                   break;
        case PrimitiveOpType::Minus:         op = Microsoft::MSR::CNTK::ElementWiseOperator::opDifference;            break;
        case PrimitiveOpType::ElementTimes:  op = Microsoft::MSR::CNTK::ElementWiseOperator::opElementwiseProduct;    break;
        case PrimitiveOpType::LogPlus:       op = Microsoft::MSR::CNTK::ElementWiseOperator::opLogSum;                break; // this is LogAddExp()
        case PrimitiveOpType::Pow:           op = Microsoft::MSR::CNTK::ElementWiseOperator::opPow;                   break;
        case PrimitiveOpType::Equal:         op = Microsoft::MSR::CNTK::ElementWiseOperator::opEqual;                 break;
        case PrimitiveOpType::NotEqual:      op = Microsoft::MSR::CNTK::ElementWiseOperator::opNotEqual;              break;
        case PrimitiveOpType::Less:          op = Microsoft::MSR::CNTK::ElementWiseOperator::opLess;                  break;
        case PrimitiveOpType::LessEqual:     op = Microsoft::MSR::CNTK::ElementWiseOperator::opLessEqual;             break;
        case PrimitiveOpType::Greater:       op = Microsoft::MSR::CNTK::ElementWiseOperator::opGreater;               break;
        case PrimitiveOpType::GreaterEqual:  op = Microsoft::MSR::CNTK::ElementWiseOperator::opGreaterEqual;          break;
            // unary elementwise ops as well are done outside, we just set the opcode
        case PrimitiveOpType::ReLU:          op = Microsoft::MSR::CNTK::ElementWiseOperator::opLinearRectifier;       break;
        case PrimitiveOpType::Tanh:          op = Microsoft::MSR::CNTK::ElementWiseOperator::opTanh;                  break;
        case PrimitiveOpType::Sigmoid:       op = Microsoft::MSR::CNTK::ElementWiseOperator::opSigmoid;               break;
        case PrimitiveOpType::Log:           op = Microsoft::MSR::CNTK::ElementWiseOperator::opLog;                   break;
        case PrimitiveOpType::Exp:           op = Microsoft::MSR::CNTK::ElementWiseOperator::opExp;                   break;
        case PrimitiveOpType::Cos:           op = Microsoft::MSR::CNTK::ElementWiseOperator::opCosine;                break;
        case PrimitiveOpType::Sin:           op = Microsoft::MSR::CNTK::ElementWiseOperator::opSin;                   break;
        case PrimitiveOpType::Negate:        op = Microsoft::MSR::CNTK::ElementWiseOperator::opNegate;                break;
        case PrimitiveOpType::Floor:         op = Microsoft::MSR::CNTK::ElementWiseOperator::opFloor;                 break;
        case PrimitiveOpType::Abs:           op = Microsoft::MSR::CNTK::ElementWiseOperator::opAbs;                   break;
        case PrimitiveOpType::Sqrt:          op = Microsoft::MSR::CNTK::ElementWiseOperator::opSqrt;                  break;
        case PrimitiveOpType::Reciprocal:    op = Microsoft::MSR::CNTK::ElementWiseOperator::opReciprocal;            break;
        case PrimitiveOpType::ELU:           op = Microsoft::MSR::CNTK::ElementWiseOperator::opExponentialLinearUnit; break;
        case PrimitiveOpType::StableSigmoid: op = Microsoft::MSR::CNTK::ElementWiseOperator::opStableSigmoid;         break;
            // ternary operations
        case PrimitiveOpType::Clip:          op = Microsoft::MSR::CNTK::ElementWiseOperator::opClip;                  break;
        case PrimitiveOpType::Select:        op = Microsoft::MSR::CNTK::ElementWiseOperator::opCond;                  break;
            // Slice if copy requested or needed
        case PrimitiveOpType::Slice:
            // The slice view has already completed, but we must copy the result over. The following op is the same as the shared one except for taking the slice view as its input.
            NDArrayView::NumericOperation({ sliceView }, alpha, Microsoft::MSR::CNTK::ElementWiseOperator::opCopy, out, 0.0, reductionOp);
            break;
            // reduction ops are also done outside, but set the reductionOp
        case PrimitiveOpType::ReduceElements:
            {
                op = Microsoft::MSR::CNTK::ElementWiseOperator::opCopy; // note: reduction axes already fully specified via outputShape
                const auto& reductionOpName = attributes[PrimitiveFunction::AttributeNameReductionOpName].Value<wstring>();
                if (reductionOpName == PrimitiveFunction::InternalSumReductionOpName)
                    reductionOp = Microsoft::MSR::CNTK::ElementWiseOperator::opSum;
                else if (reductionOpName == PrimitiveFunction::InternalLogSumReductionOpName)
                    reductionOp = Microsoft::MSR::CNTK::ElementWiseOperator::opLogSum;
                else if (reductionOpName == PrimitiveFunction::InternalMeanReductionOpName)
                {
                    reductionOp = Microsoft::MSR::CNTK::ElementWiseOperator::opSum;
                    alpha = (double)outputShape.TotalSize() / (double)args[0]->Shape().TotalSize();
                }
                else
                    //  PrimitiveFunction::InternalMaxReductionOpName
                    //  PrimitiveFunction::InternalMinReductionOpName
                    //  PrimitiveFunction::InternalProdReductionOpName
                    LogicError("Variable '%S' Value(): Reduction op %S not yet implemented.", funcForErrMsg.AsString().c_str(), reductionOpName.c_str());
            }
            break;
            // non-elementwise ops are done here
        case PrimitiveOpType::Times:
        case PrimitiveOpType::TransposeTimes:
            out->MatrixProduct(false, args[0], primitiveOp == PrimitiveOpType::TransposeTimes, args[1], false, 1.0, attributes[PrimitiveFunction::AttributeNameOutputRank].Value<size_t>(), out);
            break;
        case PrimitiveOpType::Splice:
            {
                let& axis = attributes[PrimitiveFunction::AttributeNameAxis].Value<Axis>();
                size_t maxInputRank = args[0]->Shape().Rank();
                for (int i = 1; i < args.size(); i++)
                {
                    auto inputRank = args[i]->Shape().Rank();
                    if (maxInputRank < inputRank)
                        maxInputRank = inputRank;
                }
                //NormalizeStaticAxis(axis, NDShape(maxInputRank)); // already done in InferOutputs()
                // BUGBUG: This can only splice along the last axis for now.
                if (axis.StaticAxisIndex() != outputShape.Rank() - 1)
                    LogicError("Variable '%S' Value(): Memoziation of splice along axis other than last is not implemented yet.", funcForErrMsg.AsString().c_str());
                if (args.size() > 1)
                    NDArrayView::GatherBatch(args, axis.StaticAxisIndex(), out);
                else // only one: do nothing or at best reshape if a new axis is added
                {
                    // BUGBUG: This is a 'free' op, should be caught earlier.
                    out = args[0];
                    if (out->Shape() != outputShape)
                        out = out->AsShape(outputShape);
                }
            }
            break;
            // the following N-nary operations should be easy, mostly a matter of writing tests
            // unary operations to be completed
        case PrimitiveOpType::TransposeAxes:
            // This is not hard but different from the above ops, in that it requires manipulating the TensorShape.
            // Basically we need to create a transposed view on the arg, and then do an opCopy to bring it into dense format again.
            LogicError("Variable '%S' Value(): Memoziation of unary operator %S not implemented yet.", funcForErrMsg.AsString().c_str(), PrimitiveOpTypeName(primitiveOp).c_str());
            LogicError("Variable '%S' Value(): Memoziation of ternary operator %S not implemented yet.", funcForErrMsg.AsString().c_str(), PrimitiveOpTypeName(primitiveOp).c_str());
            // the following operations are not TensorView, and may be implementable through relatively simple calls to Matrix
        case PrimitiveOpType::BatchNormalization:
            // batch normalization is a little tricky
            {
                if (args.size() != 9)
                    LogicError("Variable '%S' Value(): Operation %S requires 3 additional arguments.", funcForErrMsg.AsString().c_str(), PrimitiveOpTypeName(primitiveOp).c_str());
                let& arg = args[0];
                let& scale = args[1];
                let& bias = args[2];
                // BUGBUG: TODO: implement aggregation of stats
                // the following three are temps that carry over to backprop
                let& meanTmp    = args[6]; // mu
                let& sigmaTmp   = args[7]; // sigma
                let& normedTmp  = args[8]; // (x-mu)/sigma
                // mu and sigma
                NDArrayView::NumericOperation({ arg }, (double)meanTmp->Shape().TotalSize() / (double)arg->Shape().TotalSize(), Microsoft::MSR::CNTK::ElementWiseOperator::opCopy, meanTmp);
                NDArrayView::NumericOperation({ arg, meanTmp }, (double)sigmaTmp->Shape().TotalSize() / (double)arg->Shape().TotalSize(), Microsoft::MSR::CNTK::ElementWiseOperator::opSqrOfDifference, sigmaTmp); // sigma^2
                NDArrayView::NumericOperation({ sigmaTmp }, 1.0, Microsoft::MSR::CNTK::ElementWiseOperator::opSqrt, sigmaTmp); // sigma (in-place)
                double epsilon = attributes[PrimitiveFunction::AttributeNameEpsilon].Value<double>();
                if (epsilon > 0) // we add eps to sigma to avoid dividing by 0 or very small estimates
                    NDArrayView::NumericOperation({ }, epsilon, Microsoft::MSR::CNTK::ElementWiseOperator::opConstOne, sigmaTmp, /*beta=*/1.0); // sigma + eps (in-place)
                // normedTmp = (x-mu)/sigma
                NDArrayView::NumericOperation({ arg, sigmaTmp, meanTmp }, 1.0, Microsoft::MSR::CNTK::ElementWiseOperator::opAminusCoverB, normedTmp);  // (x-mu)/sigma
                // apply scale and bias
                NDArrayView::NumericOperation({ normedTmp, scale, bias }, 1.0, Microsoft::MSR::CNTK::ElementWiseOperator::opAxBplusC, out);
            }
            break;
        case PrimitiveOpType::OneHot:
            LogicError("Variable '%S' Value(): Memoziation of operation %S not implemented yet.", funcForErrMsg.AsString().c_str(), PrimitiveOpTypeName(primitiveOp).c_str());
            // the following operations are not TensorView, and hence should be routed through V1 ComputationNodes
            // convolution family
        case PrimitiveOpType::Convolution:  // TODO: route these through TensorView
        case PrimitiveOpType::Pooling:
        case PrimitiveOpType::Unpooling:
        case PrimitiveOpType::ROIPooling:
            // random family
        case PrimitiveOpType::RandomSample:
        case PrimitiveOpType::RandomSampleInclusionFrequency:
            // --- ops below are those that do not apply to Dynamite and therefore are not supported for direct evaluation
            // dynamic-axis related operations are not supported as dynamic axes require Inputs and are therefore not applicable here
        case PrimitiveOpType::Gather:
        case PrimitiveOpType::PackedIndex:
        case PrimitiveOpType::GatherPacked:
        case PrimitiveOpType::ScatterPacked:
        case PrimitiveOpType::PastValue:
        case PrimitiveOpType::FutureValue:
        case PrimitiveOpType::Where:
        case PrimitiveOpType::OptimizedRNNStack:
        case PrimitiveOpType::ReconcileDynamicAxis:
        case PrimitiveOpType::ToSequence:
        case PrimitiveOpType::ToSequenceLike:
        case PrimitiveOpType::UnpackSequence:
            RuntimeError("Variable '%S' Value(): Memoziation of dynamic-axis related operation %S is not possible as they imply unknown inputs.", funcForErrMsg.AsString().c_str(), PrimitiveOpTypeName(primitiveOp).c_str());
            // some operations are not supported because they do not apply
        case PrimitiveOpType::Combine:  // TODO: should be trivial to support, just need a test
        case PrimitiveOpType::Block:    // TODO: recursively invoke, needs a test and investigation whether blocks are always singleton copies
        case PrimitiveOpType::Assign:
            RuntimeError("Variable '%S' Value(): Memoziation of operation %S not applicable (applies only to static graphs).", funcForErrMsg.AsString().c_str(), PrimitiveOpTypeName(primitiveOp).c_str());
            // special ops family
        case PrimitiveOpType::LambdaRank:
        case PrimitiveOpType::NDCG:
        case PrimitiveOpType::EditDistanceError:
        case PrimitiveOpType::LabelsToGraph:
        case PrimitiveOpType::ForwardBackward:
        case PrimitiveOpType::CosDistanceWithNegativeSamples:
            LogicError("Variable '%S' Value(): Memoziation of operation %S not implemented yet.", funcForErrMsg.AsString().c_str(), PrimitiveOpTypeName(primitiveOp).c_str());
        case PrimitiveOpType::Softmax:
        case PrimitiveOpType::LogSoftmax:
        case PrimitiveOpType::Hardmax:
        case PrimitiveOpType::Dropout:     // should generate as a mul with a random output
        case PrimitiveOpType::CrossEntropyWithSoftmax:
        case PrimitiveOpType::ClassificationError:
        case PrimitiveOpType::Logistic:
        case PrimitiveOpType::SquaredError:
        case PrimitiveOpType::CosDistance: // should generate discretely
        case PrimitiveOpType::SumAll:      // never generated by API
            LogicError("Variable '%S' Value(): Memoziation of operation %S not supported on this level. This should be generated differently from the outer layer.", funcForErrMsg.AsString().c_str(), PrimitiveOpTypeName(primitiveOp).c_str());
        default:
            LogicError("Variable '%S' Value(): Memoziation of non-existent operation %S?", funcForErrMsg.AsString().c_str(), PrimitiveOpTypeName(primitiveOp).c_str());
        }
        // most common case: elementwise ops are done here instead
        if (op != Microsoft::MSR::CNTK::ElementWiseOperator::opNone)
            NDArrayView::NumericOperation(args, alpha, op, out, 0.0, reductionOp);
        //out->LogToFile(PrimitiveOpTypeName(primitiveOp), stderr);
        return out;
    }

    // perform back propagation into an input
    // For now only defined for functions with 1 output.
    // Gradient must have been allocated to the correct shape already.
    // If beta == 0 then gradient can be uninitialized memory.
    // Important: Beta is meant to apply to the *entire* gradient tensor. Specifically, also for the case of Slice(),
    // beta == 0 will set the *entire* gradient tensor to 0, not just the slice.
    // Hence, when back-propagating into multiple slices of the same matrix, only pass beta=0 for the first one.
    // TODO: decide whether we pass raw pointers or shared pointers...
    /*static*/ void PrimitiveFunction::BackpropTo(const NDArrayView* outputGradient,                         // incoming gradient from top...
                              size_t i, PrimitiveOpType primitiveOp, const Dictionary& attributes,           // ...goes through this backprop function...
                              const NDArrayView* outputValue, const vector<const NDArrayView*>& inputValues, // ...using these values from forward pass...
                              const NDArrayViewPtr& gradient, double beta,                                   // ...into here. (Despite 'const', *gradient is the output.)
                              const PrimitiveFunction& funcForErrMsg)
    {
        // The majority of operators are handled by shared code after the switch statement, based on the following op-code variables.
        // Special cases do operations inside the cases themselves, and leave the opcodes untouched.
        bool handled = false; // set this for gradients that do not use the shared code at the end
        bool op0 = false; // opcode to indicate that the gradient is zero
        auto op1Arg  = Microsoft::MSR::CNTK::ElementWiseOperator::opNone; // opcode for single-argument gradients
        auto op2Args = Microsoft::MSR::CNTK::ElementWiseOperator::opNone; // and this for 2-arg ops
        auto op3Args = Microsoft::MSR::CNTK::ElementWiseOperator::opNone; // and this for 3-arg ops; all others execute inside the switch()
        const NDArrayView* arg1 = outputGradient; // incoming gradient from top is the first arg of most gradient ops, so set it by default (some ops will change it)
        const NDArrayView* arg2 = nullptr;
        const NDArrayView* arg3 = nullptr;
        double alpha = 1;
        // NOTE: For now, this only implements the operators needed for the prototype
        switch (primitiveOp)
        {
            // binary operations with simple TensorView implementation
        case PrimitiveOpType::Plus:           op1Arg  = Microsoft::MSR::CNTK::ElementWiseOperator::opCopy; break;
        case PrimitiveOpType::Minus:          op1Arg  = Microsoft::MSR::CNTK::ElementWiseOperator::opCopy; alpha = i == 0 ? 1 : -1; break;
        case PrimitiveOpType::ElementTimes:   op2Args = Microsoft::MSR::CNTK::ElementWiseOperator::opElementwiseProduct; arg2 = inputValues[1 - i]; break;
        case PrimitiveOpType::LogPlus:        op3Args = Microsoft::MSR::CNTK::ElementWiseOperator::opElementwiseProductWithLogSumDerivative; arg2 = inputValues[1 - i]; arg3 = inputValues[i]; break;
        case PrimitiveOpType::Pow:
            if (i == 0)
            {
                op3Args = Microsoft::MSR::CNTK::ElementWiseOperator::opElementwiseProductWithPowBaseDerivative;
                arg2 = inputValues[0];
                arg3 = inputValues[1];
            }
            else
            {
                op3Args = Microsoft::MSR::CNTK::ElementWiseOperator::opElementwiseProductWithPowExponentDerivative;
                arg2 = outputValue;
                arg3 = inputValues[0];
            }
            break;
            // Times family
        case PrimitiveOpType::Times:
        case PrimitiveOpType::TransposeTimes:
            arg2 = inputValues[1 - i];
            if (i == 0) // left input
                NDArrayView::MatrixProduct(/*transC=*/primitiveOp == PrimitiveOpType::TransposeTimes,
                                          /*A=*/const_cast<NDArrayView*>(arg1)->shared_from_this(), /*transA=*/false,
                                          /*B=*/const_cast<NDArrayView*>(arg2)->shared_from_this(), /*transB=*/true,  alpha, /*outputRank dummy=*/0, /*C=*/gradient, beta);
            else // right input
                NDArrayView::MatrixProduct(/*transC=*/false,
                                          /*A=*/const_cast<NDArrayView*>(arg2)->shared_from_this(), /*transA=*/primitiveOp != PrimitiveOpType::TransposeTimes,
                                          /*B=*/const_cast<NDArrayView*>(arg1)->shared_from_this(), /*transB=*/false, alpha, /*outputRank dummy=*/0, /*C=*/gradient, beta);
            //gradient->LogToFile(L"times grad", stderr);
            handled = true;
            break;
            // unary operations with simple TensorView implementation
        case PrimitiveOpType::ReLU:          op2Args = Microsoft::MSR::CNTK::ElementWiseOperator::opElementwiseProductWithLinearRectifierDerivativeFromOutput;       arg2 = outputValue; break;
        case PrimitiveOpType::Tanh:          op2Args = Microsoft::MSR::CNTK::ElementWiseOperator::opElementwiseProductWithTanhDerivativeFromOutput;                  arg2 = outputValue; break;
        case PrimitiveOpType::Sigmoid:       op2Args = Microsoft::MSR::CNTK::ElementWiseOperator::opElementwiseProductWithSigmoidDerivativeFromOutput;               arg2 = outputValue; break;
        case PrimitiveOpType::Log:           op2Args = Microsoft::MSR::CNTK::ElementWiseOperator::opElementwiseProductWithLogDerivativeFromOutput;                   arg2 = outputValue; break;
        case PrimitiveOpType::Exp:           op2Args = Microsoft::MSR::CNTK::ElementWiseOperator::opElementwiseProduct;                                              arg2 = outputValue; break;
        case PrimitiveOpType::Cos:           op2Args = Microsoft::MSR::CNTK::ElementWiseOperator::opElementwiseProductWithCosDerivative;                             arg2 = inputValues[0]; break;
        case PrimitiveOpType::Sin:           op2Args = Microsoft::MSR::CNTK::ElementWiseOperator::opElementwiseProductWithSinDerivative;                             arg2 = inputValues[0]; break;
        case PrimitiveOpType::Negate:        op1Arg  = Microsoft::MSR::CNTK::ElementWiseOperator::opNegate;                                                          break;
        case PrimitiveOpType::Abs:           op2Args = Microsoft::MSR::CNTK::ElementWiseOperator::opElementwiseProductWithAbsDerivative;                             arg2 = inputValues[0]; break;
        case PrimitiveOpType::Sqrt:          op2Args = Microsoft::MSR::CNTK::ElementWiseOperator::opElementwiseProductWithSqrtDerivative;                            arg2 = outputValue; break;
        case PrimitiveOpType::Reciprocal:    op2Args = Microsoft::MSR::CNTK::ElementWiseOperator::opElementwiseProductWithReciprocalDerivative;                      arg2 = outputValue; break;
        case PrimitiveOpType::ELU:           op2Args = Microsoft::MSR::CNTK::ElementWiseOperator::opElementwiseProductWithExponentialLinearUnitDerivativeFromOutput; arg2 = outputValue; break;
        case PrimitiveOpType::StableSigmoid: op2Args = Microsoft::MSR::CNTK::ElementWiseOperator::opElementwiseProductWithSigmoidDerivativeFromOutput;               arg2 = outputValue; break;
            // no-op operations with simple TensorView implementation
            // NOTE: These do not need any data copy if there is only one consumer, which we won't know here. That case will be caught in the batched version.
        case PrimitiveOpType::NoOp:          op1Arg  = Microsoft::MSR::CNTK::ElementWiseOperator::opCopy; break;
        case PrimitiveOpType::Reshape:
            // shapes won't match, so reshape one to match the other (we can't just opCopy with mismatching shapes, which may cause weird broadcasting weirdness that won't crash but produce garbage values)
            if (arg1->Shape() != gradient->Shape())
            {
                NDArrayView::NumericOperation({ arg1->AsShape(gradient->Shape()) }, alpha, // differs from shared code below in passing the reshaped arg1
                                              Microsoft::MSR::CNTK::ElementWiseOperator::opCopy, gradient, beta);
                handled = true;
            }
            else
                op1Arg  = Microsoft::MSR::CNTK::ElementWiseOperator::opCopy; // (no shape change: they match already, use the the shared code below)
            break;
            // gradients that are copies with broadcasting
        case PrimitiveOpType::ReduceElements:
            {
                const auto& reductionOpName = attributes[PrimitiveFunction::AttributeNameReductionOpName].Value<wstring>();
                if (reductionOpName == PrimitiveFunction::InternalSumReductionOpName)
                    op1Arg = Microsoft::MSR::CNTK::ElementWiseOperator::opCopy;
                else if (reductionOpName == PrimitiveFunction::InternalLogSumReductionOpName)
                    NDArrayView::NumericOperation({ const_cast<NDArrayView*>(outputGradient )->shared_from_this(),
                                                    const_cast<NDArrayView*>( inputValues[0])->shared_from_this(),
                                                    const_cast<NDArrayView*>(outputValue    )->shared_from_this() }, alpha,
                                                  Microsoft::MSR::CNTK::ElementWiseOperator::opElementwiseProductWithExpOfDiff,
                                                  gradient, beta,
                                                  Microsoft::MSR::CNTK::ElementWiseOperator::opSum);
                else if (reductionOpName == PrimitiveFunction::InternalMeanReductionOpName)
                {
                    op1Arg = Microsoft::MSR::CNTK::ElementWiseOperator::opCopy;
                    alpha = (double)outputValue->Shape().TotalSize() / (double)inputValues[0]->Shape().TotalSize();
                }
                else
                    //  PrimitiveFunction::InternalMaxReductionOpName
                    //  PrimitiveFunction::InternalMinReductionOpName
                    //  PrimitiveFunction::InternalProdReductionOpName
                    LogicError("Variable '%S' Value(): Gradient of reduction op %S not yet implemented.", funcForErrMsg.AsString().c_str(), reductionOpName.c_str());
            }
            handled = true;
            break;
            // hard stuff
        case PrimitiveOpType::Splice:
            {
                // TODO: allow to pass index as SIZE_MAX to denote all; but only allow that for splice.
                auto axis = attributes[L"axis"/*PrimitiveFunction::AttributeNameAxis*/].Value<Axis>();
                if (axis.StaticAxisIndex() != arg1->Shape().Rank() -1)
                    LogicError("NDArrayView::GatherBatch: Currently only splicing in a new slowest-changing axis is supported.");
                NDArrayView::NumericOperation({ arg1->IndexLastAxis(i) }, alpha,
                                              Microsoft::MSR::CNTK::ElementWiseOperator::opCopy, gradient, beta);
                handled = true;
            }
            break;
        case PrimitiveOpType::Slice:
            // Backprop into the input slice of the input gradient: We can use forward-prop to determine the slice.
            if (beta == 0) // if beta = 0 then we must explicitly initialize the entire gradient matrix, not just the slice
                gradient->SetValue(0.0f);
            NDArrayView::NumericOperation({ const_cast<NDArrayView*>(arg1)->shared_from_this() }, alpha,
                                          Microsoft::MSR::CNTK::ElementWiseOperator::opCopy,
                                          GetSliceView(gradient, attributes, outputValue->Shape(), /*readOnly=*/false, funcForErrMsg),
                                          beta); // keep beta; although we just cleared it, beta=0 avoids the memory access
            // This ^^ op is the same as the shared one, except for the output slice.
            handled = true;
            break;
        case PrimitiveOpType::Clip:
            if (i == 0)
            {
                op3Args = Microsoft::MSR::CNTK::ElementWiseOperator::opCopyIfEqual;
                arg1 = inputValues[0];
                arg2 = outputValue;
                arg3 = outputGradient;
            }
            else // the bounds have no gradient
                op0 = true;
            break;
        case PrimitiveOpType::Select:
            if (i == 0) // the condition has no gradient
                op0 = true;
            else
            {
                if (i == 1)
                    op2Args = Microsoft::MSR::CNTK::ElementWiseOperator::opCopyIf;
                else
                    op2Args = Microsoft::MSR::CNTK::ElementWiseOperator::opCopyIfNot;
                arg1 = inputValues[0];
                arg2 = outputGradient;
            }
            break;
            // primitives that have zero gradient (piecewise constants)
        case PrimitiveOpType::Equal:
        case PrimitiveOpType::NotEqual:
        case PrimitiveOpType::Less:
        case PrimitiveOpType::LessEqual:
        case PrimitiveOpType::Greater:
        case PrimitiveOpType::GreaterEqual:
        case PrimitiveOpType::Floor:
            op0 = true; // will be set to zero below
            break;
        case PrimitiveOpType::BatchNormalization:
            if (i == 0) // input argument
            {
                let& scale     = inputValues[1];
                let& sigmaTmp  = inputValues[7]; // sigma
                let& normedTmp = inputValues[8]; // t = (x-mu)/sigma
                let N = (double)inputValues[0]->Shape().TotalSize() / (double)sigmaTmp->Shape().TotalSize();
                // cf. CNTK engine:
                //gradient += (1 / N) * (outputGradient * scale / sigmaTmp) * (N - 1 - normedTmp2);
                // t^2 = (x-mu)^2/sigma^2
                let normedTmp2 = NDArrayView::NumericOperation({ const_cast<NDArrayView*>(normedTmp)->shared_from_this() }, 1.0, Microsoft::MSR::CNTK::ElementWiseOperator::opSqr);
                //sigmaTmp->LogToFile(L"sigma");
                //normedTmp->LogToFile(L"normedTmp");
                //normedTmp2->LogToFile(L"normedTmp^2");
                // (N - 1 - t^2)
                NDArrayView::NumericOperation({}, /*alpha=*/N - 1, Microsoft::MSR::CNTK::ElementWiseOperator::opConstOne, normedTmp2, /*beta=*/-1.0);
                //normedTmp2->LogToFile(L"N - 1 -normedTmp^2");
                // gradient from top * scale / sigma
                let gScOverSigma = NDArrayView::NumericOperation({ const_cast<NDArrayView*>(outputGradient)->shared_from_this(),       // multiplied
                                                                   const_cast<NDArrayView*>(scale)->shared_from_this(),      // multiplied
                                                                   const_cast<NDArrayView*>(sigmaTmp)->shared_from_this() }, // divided by
                                                                 1.0,
                                                                 Microsoft::MSR::CNTK::ElementWiseOperator::opElementwiseProductWithQuotient);
                // add to gradient
                NDArrayView::NumericOperation({ gScOverSigma, normedTmp2 }, /*alpha=*/1 / N, Microsoft::MSR::CNTK::ElementWiseOperator::opElementwiseProduct, gradient, beta);
                handled = true;
            }
            else if (i == 1) // scale
            {
                let& normedTmp = inputValues[8]; // (x-mu)/sigma
                NDArrayView::NumericOperation({ const_cast<NDArrayView*>(arg1)->shared_from_this(),
                                                const_cast<NDArrayView*>(normedTmp)->shared_from_this() },
                                              /*alpha=*/1.0,
                                              Microsoft::MSR::CNTK::ElementWiseOperator::opElementwiseProduct, gradient, beta);
                //gradient->LogToFile(L"-> bn scale gradient", stderr);
                handled = true;
            }
            else if (i == 2) // bias
            {
                op1Arg = Microsoft::MSR::CNTK::ElementWiseOperator::opCopy;
            }
            else
                op0 = true; // no gradients except for
            break;
        default:
            //fprintf(stderr, "NEEDS: %S\n", PrimitiveOpTypeName(primitiveOp).c_str());
            LogicError("Variable '%S' Value(): Backpropagation for operation %S not implemented yet.", funcForErrMsg.AsString().c_str(), PrimitiveOpTypeName(primitiveOp).c_str());
            //LogicError("Variable '%S' Value(): Backpropagation for non-existent operation %S?", funcForErrMsg.AsString().c_str(), PrimitiveOpTypeName(primitiveOp).c_str());
        }
        // interpret the opcodes
        // An opcode must be set, or 'handled' must be set when the gradient was already completed in the case above.
        // TODO: we can eliminate the vector<> by passing a std::function, possibly?
        if (op0) // gradient is zero
        {
            if (beta == 0)
                gradient->SetValue(0.0f);
            else if (beta != 1) // will this ever be needed?
                LogicError("Variable '%S' Value(): Backpropagation with beta != 0 or 1 not implemented yet (operation %S).", funcForErrMsg.AsString().c_str(), PrimitiveOpTypeName(primitiveOp).c_str());
        }
        else if (op1Arg != Microsoft::MSR::CNTK::ElementWiseOperator::opNone) // gradient is a TensorView op with one operand
            NDArrayView::NumericOperation({ const_cast<NDArrayView*>(arg1)->shared_from_this() }, alpha,
                op1Arg, gradient, beta,
                Microsoft::MSR::CNTK::ElementWiseOperator::opSum);
        else if (op2Args != Microsoft::MSR::CNTK::ElementWiseOperator::opNone) // gradient is a TensorView op with two operands
            NDArrayView::NumericOperation({ const_cast<NDArrayView*>(arg1)->shared_from_this(), const_cast<NDArrayView*>(arg2)->shared_from_this() }, alpha,
                op2Args, gradient, beta,
                Microsoft::MSR::CNTK::ElementWiseOperator::opSum);
        else if (op3Args != Microsoft::MSR::CNTK::ElementWiseOperator::opNone) // gradient is a TensorView op with three operands
            NDArrayView::NumericOperation({ const_cast<NDArrayView*>(arg1)->shared_from_this(), const_cast<NDArrayView*>(arg2)->shared_from_this(), const_cast<NDArrayView*>(arg3)->shared_from_this() }, alpha,
                op3Args, gradient, beta,
                Microsoft::MSR::CNTK::ElementWiseOperator::opSum);
        else if (!handled)
            LogicError("Variable '%S' Value(): Gradient for operation %S misses a handler.", funcForErrMsg.AsString().c_str(), PrimitiveOpTypeName(primitiveOp).c_str());
    }

}