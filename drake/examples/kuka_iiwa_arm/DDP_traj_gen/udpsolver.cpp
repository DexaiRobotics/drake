#include "udpsolver.h"

/* Debug */
#include <iostream>
using namespace std;
/* */

#include <mutex>
std::mutex mtx;

using namespace Eigen;
using Eigen::VectorXd;

namespace drake {
namespace examples {
namespace kuka_iiwa_arm {
namespace {

UDPSolver::UDPSolver(KukaArm& iiwaDynamicModel, CostFunctionKukaArm& iiwaCostFunction, bool fullDDP, bool QPBox)
{
    //TRACE_UDP("initialize dynamic model and cost function\n");
    dynamicModel = &iiwaDynamicModel;
    costFunction = &iiwaCostFunction;
    stateNb = iiwaDynamicModel.getStateNb();
    commandNb = iiwaDynamicModel.getCommandNb();
    enableQPBox = QPBox;
    enableFullDDP = fullDDP;

    if(enableQPBox) TRACE_UDP("Box QP is enabledDD\n");
    else TRACE_UDP("Box QP is disabled\n");

    if(enableFullDDP) TRACE_UDP("Full DDP is enabled\n");
    else TRACE_UDP("Full DDP is disabled\n");

    // if(QPBox)
    // {
    //     qp = new QProblemB(commandNb);
    //     Options iiwaOptions;
    //     iiwaOptions.printLevel = PL_LOW;
    //     iiwaOptions.enableRegularisation = BT_TRUE;
    //     iiwaOptions.initialStatusBounds = ST_INACTIVE;
    //     iiwaOptions.numRefinementSteps = 1;
    //     iiwaOptions.enableCholeskyRefactorisation = 1;
    //     qp->setOptions(iiwaOptions);

    //     xOpt = new real_t[commandNb];
    //     lowerCommandBounds = iiwaDynamicModel.getLowerCommandBounds();
    //     upperCommandBounds = iiwaDynamicModel.getUpperCommandBounds();
    // }

    //tOptSet Op = INIT_OPTSET;
}

void UDPSolver::firstInitSolver(stateVec_t& iiwaxInit, stateVec_t& iiwaxgoal, unsigned int& iiwaN,
                       double& iiwadt, double& iiwascale, unsigned int& iiwamax_iter, double& iiwatolFun, double& iiwatolGrad)
{
    // TODO: double check opt params
    xInit = iiwaxInit; // removed iiwaxgoal. Double check whether this makes sense.
    xgoal = iiwaxgoal;
    N = iiwaN;
    dt = iiwadt;
    scale = iiwascale;

    //TRACE_UDP("initialize option parameters\n");
    //Op = INIT_OPTSET;
    standardizeParameters(&Op);
    Op.xInit = iiwaxInit;
    Op.n_hor = N;
    Op.tolFun = iiwatolFun;
    Op.tolGrad = iiwatolGrad;
    Op.max_iter = iiwamax_iter;

    /* computational time analysis */
    Op.time_backward.resize(Op.max_iter);
    Op.time_backward.setZero();
    Op.time_forward.resize(Op.max_iter);
    Op.time_forward.setZero();
    Op.time_derivative.resize(Op.max_iter);
    Op.time_derivative.setZero();
    Op.time_range1.resize(Op.max_iter);
    Op.time_range1.setZero();
    Op.time_range2.resize(Op.max_iter);
    Op.time_range2.setZero();
    Op.time_range3.resize(Op.max_iter);
    Op.time_range3.setZero();

    xList.resize(N+1);
    uList.resize(N);
    uListFull.resize(N+1);
    updatedxList.resize(N+1);
    updateduList.resize(N);
    costList.resize(N+1);
    costListNew.resize(N+1);
    kList.resize(N);
    KList.resize(N);
    FList.resize(N+1);    
    Vx.resize(N+1);
    Vxx.resize(N+1);

    for(unsigned int i=0;i<N;i++){
        xList[i].setZero();
        uList[i].setZero();
        uListFull[i].setZero();
        updatedxList[i].setZero();
        updateduList[i].setZero();
        costList[i] = 0;
        costListNew[i] = 0;
        kList[i].setZero();
        KList[i].setZero();
        FList[i].setZero();    
        Vx[i].setZero();
        Vxx[i].setZero();
    }
    xList[N].setZero();
    uListFull[N].setZero();
    updatedxList[N].setZero();
    costList[N] = 0;
    costListNew[N] = 0;
    FList[N].setZero();
    Vx[N].setZero();
    Vxx[N].setZero();

    k.setZero();
    K.setZero();
    dV.setZero();

    XnextThread.resize(2*fullstatecommandSize);
    for(unsigned int i=0;i<2*fullstatecommandSize;i++){
        XnextThread[i].setZero();
    }

    Xdot1.resize(2*fullstatecommandSize);
    Xdot2.resize(2*fullstatecommandSize);
    Xdot3.resize(2*fullstatecommandSize);
    Xdot4.resize(2*fullstatecommandSize);
    for(unsigned int i=0;i<2*fullstatecommandSize;i++){
        Xdot1[i].setZero();
        Xdot2[i].setZero();
        Xdot3[i].setZero();
        Xdot4[i].setZero();
    }

    // parameters for line search
    Op.alphaList.resize(11);
    Op.alphaList << 1.0, 0.5012, 0.2512, 0.1259, 0.0631, 0.0316, 0.0158, 0.0079, 0.0040, 0.0020, 0.0010;

    debugging_print = 0;
    thread.resize(NUMBER_OF_THREAD);
    enable_rk4_ = 1;
    enable_euler_ = 0;

    // initialization in doBackwardPass
    augMatrix.resize(fullstatecommandSize, fullstatecommandSize);
    Sig.resize(fullstatecommandSize, 2*fullstatecommandSize);
    augState.resize(fullstatecommandSize, 1);
    G.resize(2*fullstatecommandSize, 1);
    D.resize(fullstatecommandSize, fullstatecommandSize);
    df.resize(fullstatecommandSize, 1);
    M.resize(fullstatecommandSize, fullstatecommandSize);
    HH.resize(fullstatecommandSize, fullstatecommandSize);
    ZeroLowerLeftMatrix.setZero();
    ZeroUpperRightMatrix.setZero();
    Vxx_next_inverse.setZero();
    cuu_inverse.setZero();
}

void UDPSolver::solveTrajectory()
{
    initializeTraj();
    
    Op.lambda = Op.lambdaInit;
    Op.dlambda = Op.dlambdaInit;
    
    // TODO: update multipliers
    //update_multipliers(Op, 1);

    for(iter=0;iter<Op.max_iter;iter++)
    {
        //TRACE_UDP("STEP 1: differentiate cost along new trajectory\n");
        if(newDeriv){
            int nargout = 7;//fx,fu,cx,cu,cxx,cxu,cuu
            for(unsigned int i=0;i<u_NAN.size();i++)
                u_NAN(i,0) = sqrt(-1.0);
            for(unsigned int i=0;i<uList.size();i++)
                uListFull[i] = uList[i];

            uListFull[uList.size()] = u_NAN;

            gettimeofday(&tbegin_time_deriv,NULL);
            dynamicModel->kuka_arm_dyn_cst_udp(nargout, xList, uListFull, FList, costFunction);
            gettimeofday(&tend_time_deriv,NULL);
            Op.time_derivative(iter) = ((double)(1000.0*(tend_time_deriv.tv_sec-tbegin_time_deriv.tv_sec)+((tend_time_deriv.tv_usec-tbegin_time_deriv.tv_usec)/1000.0)))/1000.0;
            newDeriv = 0;
        }

        //TRACE_UDP("STEP 2: backward pass, compute optimal control law and cost-to-go\n");
        backPassDone = 0;
        while(!backPassDone){
            gettimeofday(&tbegin_time_bwd,NULL);
            doBackwardPass();
            gettimeofday(&tend_time_bwd,NULL);
            Op.time_backward(iter) = ((double)(1000.0*(tend_time_bwd.tv_sec-tbegin_time_bwd.tv_sec)+((tend_time_bwd.tv_usec-tbegin_time_bwd.tv_usec)/1000.0)))/1000.0;

            //TRACE_UDP("handle Cholesky failure case");
            if(diverge){
                if(Op.debug_level > 2) printf("Cholesky failed at timestep %d.\n",diverge);
                Op.dlambda   = max(Op.dlambda * Op.lambdaFactor, Op.lambdaFactor);
                Op.lambda    = max(Op.lambda * Op.dlambda, Op.lambdaMin);
                if(Op.lambda > Op.lambdaMax) break;

                    continue;
            }
            backPassDone = 1;
        }

        // check for termination due to small gradient
        // TODO: add constraint tolerance check
        if(Op.g_norm < Op.tolGrad && Op.lambda < 1e-5){
            Op.dlambda= min(Op.dlambda / Op.lambdaFactor, 1.0/Op.lambdaFactor);
            Op.lambda= Op.lambda * Op.dlambda * (Op.lambda > Op.lambdaMin);
            if(Op.debug_level>=1){
                TRACE_UDP(("\nSUCCESS: gradient norm < tolGrad\n"));
            }
            break;
        }

        //TRACE_UDP("STEP 3: line-search to find new control sequence, trajectory, cost");
        fwdPassDone = 0;
        if(backPassDone){
            gettimeofday(&tbegin_time_fwd,NULL);
            //only implement serial backtracking line-search
            for(int alpha_index = 0; alpha_index < Op.alphaList.size(); alpha_index++){
                alpha = Op.alphaList[alpha_index];

                doForwardPass();
                Op.dcost = accumulate(costList.begin(), costList.end(), 0.0) - accumulate(costListNew.begin(), costListNew.end(), 0.0);
                Op.expected = -alpha*(dV(0) + alpha*dV(1));
                double z;
                if(Op.expected > 0) {
                    z = Op.dcost/Op.expected;
                }else {
                    z = (double)(-signbit(Op.dcost));//[TODO:doublecheck]
                    TRACE_UDP("non-positive expected reduction: should not occur \n");//warning
                }
                if(z > Op.zMin){
                    fwdPassDone = 1;
                    break;
                }
            }
            if(!fwdPassDone) alpha = sqrt(-1.0);
            gettimeofday(&tend_time_fwd,NULL);
            Op.time_forward(iter) = ((double)(1000.0*(tend_time_fwd.tv_sec-tbegin_time_fwd.tv_sec)+((tend_time_fwd.tv_usec-tbegin_time_fwd.tv_usec)/1000.0)))/1000.0;
        }
        
        //TRACE_UDP("STEP 4: accept step (or not), draw graphics, print status"); 
        if (Op.debug_level > 1 && Op.last_head == Op.print_head){
            Op.last_head = 0;
            TRACE_UDP("iteration,\t cost, \t reduction, \t expected, \t gradient, \t log10(lambda) \n");
        }
        
        if(fwdPassDone){
            // print status
            if (Op.debug_level > 1){
                if(!debugging_print) printf("%-14d%-12.6g%-15.3g%-15.3g%-19.3g%-17.1f\n", iter+1, accumulate(costList.begin(), costList.end(), 0.0), Op.dcost, Op.expected, Op.g_norm, log10(Op.lambda));
                Op.last_head = Op.last_head+1;
            }

            Op.dlambda = min(Op.dlambda / Op.lambdaFactor, 1.0/Op.lambdaFactor);
            Op.lambda = Op.lambda * Op.dlambda * (Op.lambda > Op.lambdaMin);

            // accept changes
            xList = updatedxList;
            uList = updateduList;
            costList = costListNew;
            newDeriv = 1;

            // terminate ?
            // TODO: add constraint tolerance check
            if(Op.dcost < Op.tolFun) {
                if(Op.debug_level >= 1)
                    TRACE_UDP(("\nSUCCESS: cost change < tolFun\n"));

                break;
            }
        }else { // no cost improvement
            // increase lambda
            Op.dlambda = max(Op.dlambda * Op.lambdaFactor, Op.lambdaFactor);
            Op.lambda = max(Op.lambda * Op.dlambda, Op.lambdaMin);

            // if(o->w_pen_fact2>1.0) {
            //     o->w_pen_l= min(o->w_pen_max_l, o->w_pen_l*o->w_pen_fact2);
            //     o->w_pen_f= min(o->w_pen_max_f, o->w_pen_f*o->w_pen_fact2);
            //     forward_pass(o->nominal, o, 0.0, &o->cost, 1);
            // }
            
            // print status
            if(Op.debug_level >= 1){
                if(!debugging_print) printf("%-14d%-12.9s%-15.3g%-15.3g%-19.3g%-17.1f\n", iter+1, "No STEP", Op.dcost, Op.expected, Op.g_norm, log10(Op.lambda));
                Op.last_head = Op.last_head+1;
            }

            // terminate ?
            if(Op.lambda > Op.lambdaMax) {
                if(Op.debug_level >= 0)
                    TRACE_UDP(("\nEXIT: lambda > lambdaMax\n"));
                break;
            }
        }
        //cout << "final alpha value after one iteration: " << alpha << endl;
    }

    Op.iterations = iter;

    if(!backPassDone) {
        if(Op.debug_level >= 1)
            TRACE_UDP(("\nEXIT: no descent direction found.\n"));
        
        return;    
    } else if(iter >= Op.max_iter) {
        if(Op.debug_level >= 0)
            TRACE_UDP(("\nEXIT: Maximum iterations reached.\n"));
        
        return;
    }
}

void UDPSolver::initializeTraj()
{
    xList[0] = Op.xInit;
    commandVec_t zeroCommand;
    zeroCommand.setZero();
    // (low priority) TODO: implement control limit selection
    // (low priority) TODO: initialize trace data structure
    
    initFwdPassDone = 0;
    diverge = 1;
    for(int alpha_index = 0; alpha_index < Op.alphaList.size(); alpha_index++){
        alpha = Op.alphaList[alpha_index];    
        for(unsigned int i=0;i<N;i++)
        {
            uList[i] = zeroCommand;
        }
        doForwardPass();
        //simplistic divergence test
        int diverge_element_flag = 0;
        for(unsigned int i = 0; i < xList.size(); i++){
            for(unsigned int j = 0; j < xList[i].size(); j++){
                if(fabs(xList[i](j,0)) > 1e8)
                    diverge_element_flag = 1;
            }
        }
        if(!diverge_element_flag){
            diverge = 0;
            break;
        }
    }
    initFwdPassDone = 1;
    
    //constants, timers, counters
    newDeriv = 1; //flgChange
    Op.lambda= Op.lambdaInit;
    Op.w_pen_l= Op.w_pen_init_l;
    Op.w_pen_f= Op.w_pen_init_f;
    Op.dcost = 0;
    Op.expected = 0;
    Op.print_head = 6;
    Op.last_head = Op.print_head;
    if(Op.debug_level > 0) TRACE_UDP("\n=========== begin UDP ===========\n");
}

void UDPSolver::standardizeParameters(tOptSet *o) {
    o->n_alpha = 11;
    o->tolFun = 1e-4;
    o->tolConstraint = 1e-7; // TODO: to be modified
    o->tolGrad = 1e-4;
    o->max_iter = 500;
    o->lambdaInit = 1;
    o->dlambdaInit = 1;
    o->lambdaFactor = 1.6;
    o->lambdaMax = 1e10;
    o->lambdaMin = 1e-6;
    o->regType = 1;
    o->zMin = 0.0;
    o->debug_level = 2; // == verbosity in matlab code
    o->w_pen_init_l = 1.0; // TODO: to be modified
    o->w_pen_init_f = 1.0; // TODO: to be modified
    o->w_pen_max_l = 1e100;//set to INF originally
    o->w_pen_max_f = 1e100;//set to INF originally
    o->w_pen_fact1 = 4.0; // 4...10 Bertsekas p. 123
    o->w_pen_fact2 = 1.0; 
    o->print = 2;
}

void UDPSolver::doBackwardPass()
{    
    //Perform the Ricatti-Mayne backward pass with unscented transform
    if(Op.regType == 1)
        lambdaEye = Op.lambda*stateMat_t::Identity();
    else
        lambdaEye = Op.lambda*stateMat_t::Zero();

    diverge = 0;
    g_norm_sum = 0;
    Vx[N] = costFunction->getcx()[N];
    Vxx[N] = costFunction->getcxx()[N];
    dV.setZero();

    Op.time_range1(iter) = 0;
    Op.time_range2(iter) = 0;
    Op.time_range3(iter) = 0;
    for(int i=N-1;i>=0;i--){
        //Generate sigma points from Vxx(i+1) and cuu(i+1)
        ZeroLowerLeftMatrix.setZero();
        ZeroUpperRightMatrix.setZero();
        Vxx_next_inverse = Vxx[i+1].inverse();
        cuu_inverse = costFunction->getcuu()[i].inverse();

        augMatrix << Vxx_next_inverse, ZeroUpperRightMatrix, 
                ZeroLowerLeftMatrix, cuu_inverse;

        Eigen::LLT<MatrixXd> lltOfaugMatrix(augMatrix);

        Eigen::MatrixXd S = lltOfaugMatrix.matrixL(); 
        //assume augMatrix is positive definite

        //A temporary solution: check the non-PD case
        if(lltOfaugMatrix.info() == Eigen::NumericalIssue)
        {
            diverge = i;
            TRACE_UDP("Possibly non semi-positive definitie matrix!\n");
            return;
        }
        S = scale*S;
        Sig << S, -S;

        for(unsigned int j=0;j<2*fullstatecommandSize;j++){
            augState << xList[i+1], uList[i];
            Sig.col(j) += augState;
        }

        // Project Vx(i+1) onto sigma points
        for(unsigned int j=0;j<fullstatecommandSize;j++){
            G(j) = Vx[i+1].transpose()*S.col(j).head(stateSize);
            G(j+fullstatecommandSize) = -G(j);
        }

        gettimeofday(&tbegin_test,NULL);

        //Propagate sigma points through backwards dynamics
        if(UDP_BACKWARD_INTEGRATION_METHOD == 1){
            gettimeofday(&tbegin_test2,NULL);

            for(unsigned int j=0;j<4;j++){
                switch(j){
                    case 0: thread[j] = std::thread(&UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread1, this, Sig.col(j), dt, j); break;       
                    case 1: thread[j] = std::thread(&UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread2, this, Sig.col(j), dt, j); break;
                    case 2: thread[NUMBER_OF_THREAD-2] = std::thread(&UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread21, this, Sig.col(2*(NUMBER_OF_THREAD-2)), dt, 2*(NUMBER_OF_THREAD-2)); break;
                    case 3: thread[NUMBER_OF_THREAD-1] = std::thread(&UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread22, this, Sig.col(2*(NUMBER_OF_THREAD-2)+1), dt, 2*(NUMBER_OF_THREAD-2)+1); break;
                }
            }

            for(unsigned int j=2;j<NUMBER_OF_THREAD-2;j++){
                switch(2*j){
                    case 4: thread[j] = std::thread(&UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread3, this, Sig.col(2*j), Sig.col(2*j+1), dt, 2*j); break;
                    case 6: thread[j] = std::thread(&UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread4, this, Sig.col(2*j), Sig.col(2*j+1), dt, 2*j); break;
                    case 8: thread[j] = std::thread(&UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread5, this, Sig.col(2*j), Sig.col(2*j+1), dt, 2*j); break;
                    case 10: thread[j] = std::thread(&UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread6, this, Sig.col(2*j), Sig.col(2*j+1), dt, 2*j); break;
                    case 12: thread[j] = std::thread(&UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread7, this, Sig.col(2*j), Sig.col(2*j+1), dt, 2*j); break;
                    case 14: thread[j] = std::thread(&UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread8, this, Sig.col(2*j), Sig.col(2*j+1), dt, 2*j); break;
                    case 16: thread[j] = std::thread(&UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread9, this, Sig.col(2*j), Sig.col(2*j+1), dt, 2*j); break;
                    case 18: thread[j] = std::thread(&UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread10, this, Sig.col(2*j), Sig.col(2*j+1), dt, 2*j); break;
                    case 20: thread[j] = std::thread(&UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread11, this, Sig.col(2*j), Sig.col(2*j+1), dt, 2*j); break;
                    case 22: thread[j] = std::thread(&UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread12, this, Sig.col(2*j), Sig.col(2*j+1), dt, 2*j); break;
                    case 24: thread[j] = std::thread(&UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread13, this, Sig.col(2*j), Sig.col(2*j+1), dt, 2*j); break;
                    case 26: thread[j] = std::thread(&UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread14, this, Sig.col(2*j), Sig.col(2*j+1), dt, 2*j); break;
                    case 28: thread[j] = std::thread(&UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread15, this, Sig.col(2*j), Sig.col(2*j+1), dt, 2*j); break;
                    case 30: thread[j] = std::thread(&UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread16, this, Sig.col(2*j), Sig.col(2*j+1), dt, 2*j); break;
                    case 32: thread[j] = std::thread(&UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread17, this, Sig.col(2*j), Sig.col(2*j+1), dt, 2*j); break;
                    case 34: thread[j] = std::thread(&UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread18, this, Sig.col(2*j), Sig.col(2*j+1), dt, 2*j); break;
                    case 36: thread[j] = std::thread(&UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread19, this, Sig.col(2*j), Sig.col(2*j+1), dt, 2*j); break;
                    case 38: thread[j] = std::thread(&UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread20, this, Sig.col(2*j), Sig.col(2*j+1), dt, 2*j); break;
                }
            }

            gettimeofday(&tend_test2,NULL);
            Op.time_range2(iter) += ((double)(1000.0*(tend_test2.tv_sec-tbegin_test2.tv_sec)+((tend_test2.tv_usec-tbegin_test2.tv_usec)/1000.0)))/1000.0;

            gettimeofday(&tbegin_test3,NULL);
            for(unsigned int j=2*NUMBER_OF_THREAD-4;j<2*fullstatecommandSize;j++){
                 Sig.col(j).head(stateSize) = rungeKuttaStepBackward(Sig.col(j), dt, j);
            }
            gettimeofday(&tend_test3,NULL);
            Op.time_range3(iter) += ((double)(1000.0*(tend_test3.tv_sec-tbegin_test3.tv_sec)+((tend_test3.tv_usec-tbegin_test3.tv_usec)/1000.0)))/1000.0;

            for (unsigned int j = 0; j < 2; j++){
                 thread[j].join();
                 Sig.col(j).head(stateSize) = XnextThread[j];
            }

            Sig.col(2*(NUMBER_OF_THREAD-2)) = XnextThread[2*(NUMBER_OF_THREAD-2)];
            Sig.col(2*(NUMBER_OF_THREAD-2)+1) = XnextThread[2*(NUMBER_OF_THREAD-2)+1];

            for (unsigned int j = 2; j < NUMBER_OF_THREAD-2; j++){
                 thread[j].join();
                 Sig.col(2*j).head(stateSize) = XnextThread[2*j];
                 Sig.col(2*j+1).head(stateSize) = XnextThread[2*j+1];
            }

        }else if(UDP_BACKWARD_INTEGRATION_METHOD == 2){
            for(unsigned int j=0;j<2*fullstatecommandSize;j++)
                 Sig.col(j).head(stateSize) = eulerStepBackward(Sig.col(j), dt, j);
        }else if(UDP_BACKWARD_INTEGRATION_METHOD == 3){
            if (i < (int)N-1){
                for(unsigned int j=0;j<2*fullstatecommandSize;j++)
                    Sig.col(j).head(stateSize) = rungeKutta3StepBackward(Sig.col(j), uList[i+1], dt, j);
            }else{
                for(unsigned int j=0;j<2*fullstatecommandSize;j++)// the last knot point
                    Sig.col(j).head(stateSize) = rungeKuttaStepBackward(Sig.col(j), dt, j);
            }    
        }
        
        gettimeofday(&tend_test,NULL);
        Op.time_range1(iter) += ((double)(1000.0*(tend_test.tv_sec-tbegin_test.tv_sec)+((tend_test.tv_usec-tbegin_test.tv_usec)/1000.0)))/1000.0;

        //Calculate [Qu; Qx] from sigma points
        for(unsigned int j=0;j<fullstatecommandSize;j++){
            D.row(j) =  Sig.col(j).transpose() - Sig.col(j+fullstatecommandSize).transpose();
            df(j) = G(j) - G(fullstatecommandSize+j);
        }

        QxQu = D.inverse()*df;
        Qx = QxQu.head(stateSize) + costFunction->getcx()[i]; //add on one-step cost
        Qu = QxQu.tail(commandSize) + costFunction->getcu()[i]; //add on one-step cost

        mu.setZero();
        //Calculate Hessian w.r.t. [xList[i]; uList[i]] from sigma points
        for(unsigned int j=0;j<2*fullstatecommandSize;j++)
            mu += 1.0/(2.0*fullstatecommandSize)*Sig.col(j);

        M.setZero();

        for(unsigned int j=0;j<2*fullstatecommandSize;j++)
            M += (0.5/pow(scale, 2.0))*(Sig.col(j) - mu)*(Sig.col(j).transpose() - mu.transpose());

        HH = M.inverse();
        HH.block(0,0,stateSize,stateSize) += costFunction->getcxx()[i]; //add in one-step state cost for this timestep

        Qxx = HH.block(0,0,stateSize,stateSize);
        Quu = HH.block(stateSize,stateSize,commandSize,commandSize);
        Qux = HH.block(stateSize,0,commandSize,stateSize);

        if(Op.regType == 1)
            QuuF = Quu + Op.lambda*commandMat_t::Identity();

        QuuInv = QuuF.inverse();

        if(!isPositiveDefinite(Quu))
        {
            TRACE_UDP("Quu is not positive definite");
            if(Op.lambda==0.0) Op.lambda += 1e-4;
            else Op.lambda *= 10;
            backPassDone = 0;
            break;
        }

        // if(enableQPBox)
        // {
        //     //TRACE_UDP("Use Box QP");
        //     nWSR = 10; //[to be checked]
        //     H = Quu;
        //     g = Qu;
        //     lb = lowerCommandBounds - uList[i];
        //     ub = upperCommandBounds - uList[i];
        //     qp->init(H.data(),g.data(),lb.data(),ub.data(),nWSR);
        //     qp->getPrimalSolution(xOpt);
        //     k = Map<commandVec_t>(xOpt);
        //     K = -QuuInv*Qux;
        //     for(unsigned int i_cmd=0;i_cmd<commandNb;i_cmd++)
        //     {
        //         if((k[i_cmd] == lowerCommandBounds[i_cmd]) | (k[i_cmd] == upperCommandBounds[i_cmd]))
        //         {
        //             K.row(i_cmd).setZero();
        //         }
        //     }
        // }
        if(!enableQPBox)
        {
            // Cholesky decomposition by using upper triangular matrix
            //TRACE_UDP("Use Cholesky decomposition");
            Eigen::LLT<MatrixXd> lltOfQuuF(QuuF);
            Eigen::MatrixXd L = lltOfQuuF.matrixU(); 
            //assume QuuF is positive definite
            
            //A temporary solution: check the non-PD case
            if(lltOfQuuF.info() == Eigen::NumericalIssue)
                {
                    diverge = i;
                    TRACE_UDP("Possibly non semi-positive definitie matrix!\n");
                    return;
                }

            Eigen::MatrixXd L_inverse = L.inverse();
            k = - L_inverse*L.transpose().inverse()*Qu;
            K = - L_inverse*L.transpose().inverse()*Qux;
        }

        //update cost-to-go approximation
        dV(0) += k.transpose()*Qu;
        scalar_t c_mat_to_scalar;
        c_mat_to_scalar = 0.5*k.transpose()*Quu*k;
        dV(1) += c_mat_to_scalar(0,0);
        Vx[i] = Qx + K.transpose()*Quu*k + K.transpose()*Qu + Qux.transpose()*k;
        Vxx[i] = Qxx + K.transpose()*Quu*K+ K.transpose()*Qux + Qux.transpose()*K;
        Vxx[i] = 0.5*(Vxx[i] + Vxx[i].transpose());

        kList[i] = k;
        KList[i] = K;

        g_norm_max= 0.0;
        for(unsigned int j=0; j<commandSize; j++) {
            g_norm_i = fabs(kList[i](j,0)) / (fabs(uList[i](j,0))+1.0);
            if(g_norm_i > g_norm_max) g_norm_max = g_norm_i;
        }
        g_norm_sum += g_norm_max;
        
    }
    
    Op.g_norm = g_norm_sum/((double)(Op.n_hor));
}

void UDPSolver::doForwardPass()
{
    updatedxList[0] = Op.xInit;
    int nargout = 2;
    stateVec_t x_unused;
    x_unused.setZero();
    commandVec_t u_NAN;
    u_NAN << sqrt(-1.0);
    isUNan = 0;

    //[TODO: to be optimized]
    if(!initFwdPassDone){
        //TRACE("initial forward pass\n");
        for(unsigned int i=0;i<N;i++)
        {
            updateduList[i] = uList[i];
            dynamicModel->kuka_arm_dyn_cst_min_output(nargout, dt, updatedxList[i], updateduList[i], isUNan, updatedxList[i+1], costFunction);
            costList[i] = costFunction->getc();
        }
        isUNan = 1;
        dynamicModel->kuka_arm_dyn_cst_min_output(nargout, dt, updatedxList[N], u_NAN, isUNan, x_unused, costFunction);
        costList[N] = costFunction->getc();        
    }else{
        //TRACE("forward pass in STEP 3\n");
        for(unsigned int i=0;i<N;i++){
            updateduList[i] = uList[i] + alpha*kList[i] + KList[i]*(updatedxList[i]-xList[i]);
            dynamicModel->kuka_arm_dyn_cst_min_output(nargout, dt, updatedxList[i], updateduList[i], isUNan, updatedxList[i+1], costFunction);
            costListNew[i] = costFunction->getc();
        }
        isUNan = 1;
        dynamicModel->kuka_arm_dyn_cst_min_output(nargout, dt, updatedxList[N], u_NAN, isUNan, x_unused, costFunction);
        costListNew[N] = costFunction->getc();
    }
}

UDPSolver::traj UDPSolver::getLastSolvedTrajectory()
{
    lastTraj.xList = updatedxList;
    lastTraj.uList = updateduList;
    lastTraj.iter = iter;
    lastTraj.finalCost = accumulate(costList.begin(), costList.end(), 0.0);
    lastTraj.finalGrad = Op.g_norm;
    lastTraj.finalLambda = log10(Op.lambda);
    lastTraj.time_forward = Op.time_forward;
    lastTraj.time_backward = Op.time_backward;
    lastTraj.time_derivative = Op.time_derivative;
    lastTraj.time_range1 = Op.time_range1;
    lastTraj.time_range2 = Op.time_range2;
    lastTraj.time_range3 = Op.time_range3;
    return lastTraj;
}

bool UDPSolver::isPositiveDefinite(const commandMat_t & Quu)
{
    //Eigen::JacobiSVD<commandMat_t> svd_Quu (Quu, ComputeThinU | ComputeThinV);
    Eigen::VectorXcd singular_values = Quu.eigenvalues();

    for(long i = 0; i < Quu.cols(); ++i)
    {
        if (singular_values[i].real() < 0.)
        {
            TRACE_UDP("Matrix is not SDP");
            return false;
        }
    }
    return true;
}

stateVec_t UDPSolver::rungeKuttaStepBackward(stateAug_t augX, double& dt, unsigned int i){
    // Backwards 4^th order Runge-Kutta step from X_{k+1} to X_k
    Xdot1[i] = dynamicModel->kuka_arm_dynamics(augX.head(stateSize), augX.tail(commandSize));
    Xdot2[i] = dynamicModel->kuka_arm_dynamics(augX.head(stateSize) - 0.5*dt*Xdot1[i], augX.tail(commandSize));
    Xdot3[i] = dynamicModel->kuka_arm_dynamics(augX.head(stateSize) - 0.5*dt*Xdot2[i], augX.tail(commandSize));
    Xdot4[i] = dynamicModel->kuka_arm_dynamics(augX.head(stateSize) - dt*Xdot3[i], augX.tail(commandSize));
    return augX.head(stateSize) - (dt/6)*(Xdot1[i] + 2*Xdot2[i] + 2*Xdot3[i] + Xdot4[i]);
}

void UDPSolver::rungeKuttaStepBackwardThread(stateAug_t augXThread, double dt, unsigned int iThread){
    // Backwards 4^th order Runge-Kutta step from X_{k+1} to X_k
    mtx.lock();
        
    Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread1(augXThread.head(stateSize), augXThread.tail(commandSize), iThread);
    Xdot2[iThread] = dynamicModel->kuka_arm_dynamicsThread1(augXThread.head(stateSize) - 0.5*dt*Xdot1[iThread], augXThread.tail(commandSize), iThread);
    Xdot3[iThread] = dynamicModel->kuka_arm_dynamicsThread1(augXThread.head(stateSize) - 0.5*dt*Xdot2[iThread], augXThread.tail(commandSize), iThread);
    Xdot4[iThread] = dynamicModel->kuka_arm_dynamicsThread1(augXThread.head(stateSize) - dt*Xdot3[iThread], augXThread.tail(commandSize), iThread);
    XnextThread[iThread] = augXThread.head(stateSize) - (dt/6)*(Xdot1[iThread] + 2*Xdot2[iThread] + 2*Xdot3[iThread] + Xdot4[iThread]);
    mtx.unlock();
}

void UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread1(stateAug_t augXThread, double dt, unsigned int iThread){
    // (Two Sigma Points) Backwards 4^th order Runge-Kutta step from X_{k+1} to X_k
    if(enable_rk4_){
        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread1(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        Xdot2[iThread] = dynamicModel->kuka_arm_dynamicsThread1(augXThread.head(stateSize) - 0.5*dt*Xdot1[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot3[iThread] = dynamicModel->kuka_arm_dynamicsThread1(augXThread.head(stateSize) - 0.5*dt*Xdot2[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot4[iThread] = dynamicModel->kuka_arm_dynamicsThread1(augXThread.head(stateSize) - dt*Xdot3[iThread], augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - (dt/6)*(Xdot1[iThread] + 2*Xdot2[iThread] + 2*Xdot3[iThread] + Xdot4[iThread]);
        
    }else if(enable_euler_){
        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread1(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - dt*Xdot1[iThread];

    }
}

void UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread2(stateAug_t augXThread, double dt, unsigned int iThread){
    // (Two Sigma Points) Backwards 4^th order Runge-Kutta step from X_{k+1} to X_k
    if(enable_rk4_){

        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread2(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        Xdot2[iThread] = dynamicModel->kuka_arm_dynamicsThread2(augXThread.head(stateSize) - 0.5*dt*Xdot1[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot3[iThread] = dynamicModel->kuka_arm_dynamicsThread2(augXThread.head(stateSize) - 0.5*dt*Xdot2[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot4[iThread] = dynamicModel->kuka_arm_dynamicsThread2(augXThread.head(stateSize) - dt*Xdot3[iThread], augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - (dt/6)*(Xdot1[iThread] + 2*Xdot2[iThread] + 2*Xdot3[iThread] + Xdot4[iThread]);
        
    }else if(enable_euler_){
        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread1(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - dt*Xdot1[iThread];
    }
}

void UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread3(stateAug_t augXThread, stateAug_t augXThreadNext, double dt, unsigned int iThread){
    // (Two Sigma Points) Backwards 4^th order Runge-Kutta step from X_{k+1} to X_k
    if(enable_rk4_){

        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread3(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        Xdot2[iThread] = dynamicModel->kuka_arm_dynamicsThread3(augXThread.head(stateSize) - 0.5*dt*Xdot1[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot3[iThread] = dynamicModel->kuka_arm_dynamicsThread3(augXThread.head(stateSize) - 0.5*dt*Xdot2[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot4[iThread] = dynamicModel->kuka_arm_dynamicsThread3(augXThread.head(stateSize) - dt*Xdot3[iThread], augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - (dt/6)*(Xdot1[iThread] + 2*Xdot2[iThread] + 2*Xdot3[iThread] + Xdot4[iThread]);
        
        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread3(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        Xdot2[iThread+1] = dynamicModel->kuka_arm_dynamicsThread3(augXThreadNext.head(stateSize) - 0.5*dt*Xdot1[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot3[iThread+1] = dynamicModel->kuka_arm_dynamicsThread3(augXThreadNext.head(stateSize) - 0.5*dt*Xdot2[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot4[iThread+1] = dynamicModel->kuka_arm_dynamicsThread3(augXThreadNext.head(stateSize) - dt*Xdot3[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        XnextThread[iThread+1] = augXThreadNext.head(stateSize) - (dt/6)*(Xdot1[iThread+1] + 2*Xdot2[iThread+1] + 2*Xdot3[iThread+1] + Xdot4[iThread+1]);

    }else if(enable_euler_){
        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread1(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - dt*Xdot1[iThread];

        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread1(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        XnextThread[iThread+1] = augXThreadNext.head(stateSize) - dt*Xdot1[iThread+1];
    }
}

void UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread4(stateAug_t augXThread, stateAug_t augXThreadNext, double dt, unsigned int iThread){
    // (Two Sigma Points) Backwards 4^th order Runge-Kutta step from X_{k+1} to X_k
    if(enable_rk4_){
        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread4(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        Xdot2[iThread] = dynamicModel->kuka_arm_dynamicsThread4(augXThread.head(stateSize) - 0.5*dt*Xdot1[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot3[iThread] = dynamicModel->kuka_arm_dynamicsThread4(augXThread.head(stateSize) - 0.5*dt*Xdot2[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot4[iThread] = dynamicModel->kuka_arm_dynamicsThread4(augXThread.head(stateSize) - dt*Xdot3[iThread], augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - (dt/6)*(Xdot1[iThread] + 2*Xdot2[iThread] + 2*Xdot3[iThread] + Xdot4[iThread]);
        
        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread4(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        Xdot2[iThread+1] = dynamicModel->kuka_arm_dynamicsThread4(augXThreadNext.head(stateSize) - 0.5*dt*Xdot1[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot3[iThread+1] = dynamicModel->kuka_arm_dynamicsThread4(augXThreadNext.head(stateSize) - 0.5*dt*Xdot2[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot4[iThread+1] = dynamicModel->kuka_arm_dynamicsThread4(augXThreadNext.head(stateSize) - dt*Xdot3[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        XnextThread[iThread+1] = augXThreadNext.head(stateSize) - (dt/6)*(Xdot1[iThread+1] + 2*Xdot2[iThread+1] + 2*Xdot3[iThread+1] + Xdot4[iThread+1]);
    }else if(enable_euler_){
        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread1(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - dt*Xdot1[iThread];

        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread1(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        XnextThread[iThread+1] = augXThreadNext.head(stateSize) - dt*Xdot1[iThread+1];
    }
}

void UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread5(stateAug_t augXThread, stateAug_t augXThreadNext, double dt, unsigned int iThread){
    // (Two Sigma Points) Backwards 4^th order Runge-Kutta step from X_{k+1} to X_k
    if(enable_rk4_){

        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread5(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        Xdot2[iThread] = dynamicModel->kuka_arm_dynamicsThread5(augXThread.head(stateSize) - 0.5*dt*Xdot1[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot3[iThread] = dynamicModel->kuka_arm_dynamicsThread5(augXThread.head(stateSize) - 0.5*dt*Xdot2[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot4[iThread] = dynamicModel->kuka_arm_dynamicsThread5(augXThread.head(stateSize) - dt*Xdot3[iThread], augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - (dt/6)*(Xdot1[iThread] + 2*Xdot2[iThread] + 2*Xdot3[iThread] + Xdot4[iThread]);
        
        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread5(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        Xdot2[iThread+1] = dynamicModel->kuka_arm_dynamicsThread5(augXThreadNext.head(stateSize) - 0.5*dt*Xdot1[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot3[iThread+1] = dynamicModel->kuka_arm_dynamicsThread5(augXThreadNext.head(stateSize) - 0.5*dt*Xdot2[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot4[iThread+1] = dynamicModel->kuka_arm_dynamicsThread5(augXThreadNext.head(stateSize) - dt*Xdot3[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        XnextThread[iThread+1] = augXThreadNext.head(stateSize) - (dt/6)*(Xdot1[iThread+1] + 2*Xdot2[iThread+1] + 2*Xdot3[iThread+1] + Xdot4[iThread+1]);
    }else if(enable_euler_){
        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread1(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - dt*Xdot1[iThread];

        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread1(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        XnextThread[iThread+1] = augXThreadNext.head(stateSize) - dt*Xdot1[iThread+1];
    }
}

void UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread6(stateAug_t augXThread, stateAug_t augXThreadNext, double dt, unsigned int iThread){
    // (Two Sigma Points) Backwards 4^th order Runge-Kutta step from X_{k+1} to X_k
    if(enable_rk4_){

        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread6(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        Xdot2[iThread] = dynamicModel->kuka_arm_dynamicsThread6(augXThread.head(stateSize) - 0.5*dt*Xdot1[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot3[iThread] = dynamicModel->kuka_arm_dynamicsThread6(augXThread.head(stateSize) - 0.5*dt*Xdot2[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot4[iThread] = dynamicModel->kuka_arm_dynamicsThread6(augXThread.head(stateSize) - dt*Xdot3[iThread], augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - (dt/6)*(Xdot1[iThread] + 2*Xdot2[iThread] + 2*Xdot3[iThread] + Xdot4[iThread]);
        
        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread6(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        Xdot2[iThread+1] = dynamicModel->kuka_arm_dynamicsThread6(augXThreadNext.head(stateSize) - 0.5*dt*Xdot1[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot3[iThread+1] = dynamicModel->kuka_arm_dynamicsThread6(augXThreadNext.head(stateSize) - 0.5*dt*Xdot2[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot4[iThread+1] = dynamicModel->kuka_arm_dynamicsThread6(augXThreadNext.head(stateSize) - dt*Xdot3[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        XnextThread[iThread+1] = augXThreadNext.head(stateSize) - (dt/6)*(Xdot1[iThread+1] + 2*Xdot2[iThread+1] + 2*Xdot3[iThread+1] + Xdot4[iThread+1]);
    }else if(enable_euler_){
        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread1(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - dt*Xdot1[iThread];

        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread1(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        XnextThread[iThread+1] = augXThreadNext.head(stateSize) - dt*Xdot1[iThread+1];
    }
}

void UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread7(stateAug_t augXThread, stateAug_t augXThreadNext, double dt, unsigned int iThread){
    // (Two Sigma Points) Backwards 4^th order Runge-Kutta step from X_{k+1} to X_k
    if(enable_rk4_){

        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread7(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        Xdot2[iThread] = dynamicModel->kuka_arm_dynamicsThread7(augXThread.head(stateSize) - 0.5*dt*Xdot1[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot3[iThread] = dynamicModel->kuka_arm_dynamicsThread7(augXThread.head(stateSize) - 0.5*dt*Xdot2[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot4[iThread] = dynamicModel->kuka_arm_dynamicsThread7(augXThread.head(stateSize) - dt*Xdot3[iThread], augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - (dt/6)*(Xdot1[iThread] + 2*Xdot2[iThread] + 2*Xdot3[iThread] + Xdot4[iThread]);
        
        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread7(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        Xdot2[iThread+1] = dynamicModel->kuka_arm_dynamicsThread7(augXThreadNext.head(stateSize) - 0.5*dt*Xdot1[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot3[iThread+1] = dynamicModel->kuka_arm_dynamicsThread7(augXThreadNext.head(stateSize) - 0.5*dt*Xdot2[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot4[iThread+1] = dynamicModel->kuka_arm_dynamicsThread7(augXThreadNext.head(stateSize) - dt*Xdot3[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        XnextThread[iThread+1] = augXThreadNext.head(stateSize) - (dt/6)*(Xdot1[iThread+1] + 2*Xdot2[iThread+1] + 2*Xdot3[iThread+1] + Xdot4[iThread+1]);
    }else if(enable_euler_){
        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread1(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - dt*Xdot1[iThread];

        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread1(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        XnextThread[iThread+1] = augXThreadNext.head(stateSize) - dt*Xdot1[iThread+1];
    }
}

void UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread8(stateAug_t augXThread, stateAug_t augXThreadNext, double dt, unsigned int iThread){
    // (Two Sigma Points) Backwards 4^th order Runge-Kutta step from X_{k+1} to X_k
    if(enable_rk4_){

        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread8(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        Xdot2[iThread] = dynamicModel->kuka_arm_dynamicsThread8(augXThread.head(stateSize) - 0.5*dt*Xdot1[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot3[iThread] = dynamicModel->kuka_arm_dynamicsThread8(augXThread.head(stateSize) - 0.5*dt*Xdot2[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot4[iThread] = dynamicModel->kuka_arm_dynamicsThread8(augXThread.head(stateSize) - dt*Xdot3[iThread], augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - (dt/6)*(Xdot1[iThread] + 2*Xdot2[iThread] + 2*Xdot3[iThread] + Xdot4[iThread]);
        
        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread8(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        Xdot2[iThread+1] = dynamicModel->kuka_arm_dynamicsThread8(augXThreadNext.head(stateSize) - 0.5*dt*Xdot1[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot3[iThread+1] = dynamicModel->kuka_arm_dynamicsThread8(augXThreadNext.head(stateSize) - 0.5*dt*Xdot2[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot4[iThread+1] = dynamicModel->kuka_arm_dynamicsThread8(augXThreadNext.head(stateSize) - dt*Xdot3[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        XnextThread[iThread+1] = augXThreadNext.head(stateSize) - (dt/6)*(Xdot1[iThread+1] + 2*Xdot2[iThread+1] + 2*Xdot3[iThread+1] + Xdot4[iThread+1]);
    }else if(enable_euler_){
        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread1(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - dt*Xdot1[iThread];

        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread1(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        XnextThread[iThread+1] = augXThreadNext.head(stateSize) - dt*Xdot1[iThread+1];
    }
}

void UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread9(stateAug_t augXThread, stateAug_t augXThreadNext, double dt, unsigned int iThread){
    // (Two Sigma Points) Backwards 4^th order Runge-Kutta step from X_{k+1} to X_k
    if(enable_rk4_){

        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread9(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        Xdot2[iThread] = dynamicModel->kuka_arm_dynamicsThread9(augXThread.head(stateSize) - 0.5*dt*Xdot1[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot3[iThread] = dynamicModel->kuka_arm_dynamicsThread9(augXThread.head(stateSize) - 0.5*dt*Xdot2[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot4[iThread] = dynamicModel->kuka_arm_dynamicsThread9(augXThread.head(stateSize) - dt*Xdot3[iThread], augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - (dt/6)*(Xdot1[iThread] + 2*Xdot2[iThread] + 2*Xdot3[iThread] + Xdot4[iThread]);
        
        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread9(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        Xdot2[iThread+1] = dynamicModel->kuka_arm_dynamicsThread9(augXThreadNext.head(stateSize) - 0.5*dt*Xdot1[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot3[iThread+1] = dynamicModel->kuka_arm_dynamicsThread9(augXThreadNext.head(stateSize) - 0.5*dt*Xdot2[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot4[iThread+1] = dynamicModel->kuka_arm_dynamicsThread9(augXThreadNext.head(stateSize) - dt*Xdot3[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        XnextThread[iThread+1] = augXThreadNext.head(stateSize) - (dt/6)*(Xdot1[iThread+1] + 2*Xdot2[iThread+1] + 2*Xdot3[iThread+1] + Xdot4[iThread+1]);
    }else if(enable_euler_){
        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread1(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - dt*Xdot1[iThread];

        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread1(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        XnextThread[iThread+1] = augXThreadNext.head(stateSize) - dt*Xdot1[iThread+1];
    }
}

void UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread10(stateAug_t augXThread, stateAug_t augXThreadNext, double dt, unsigned int iThread){
    // (Two Sigma Points) Backwards 4^th order Runge-Kutta step from X_{k+1} to X_k
    if(enable_rk4_){

        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread10(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        Xdot2[iThread] = dynamicModel->kuka_arm_dynamicsThread10(augXThread.head(stateSize) - 0.5*dt*Xdot1[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot3[iThread] = dynamicModel->kuka_arm_dynamicsThread10(augXThread.head(stateSize) - 0.5*dt*Xdot2[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot4[iThread] = dynamicModel->kuka_arm_dynamicsThread10(augXThread.head(stateSize) - dt*Xdot3[iThread], augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - (dt/6)*(Xdot1[iThread] + 2*Xdot2[iThread] + 2*Xdot3[iThread] + Xdot4[iThread]);
        
        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread10(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        Xdot2[iThread+1] = dynamicModel->kuka_arm_dynamicsThread10(augXThreadNext.head(stateSize) - 0.5*dt*Xdot1[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot3[iThread+1] = dynamicModel->kuka_arm_dynamicsThread10(augXThreadNext.head(stateSize) - 0.5*dt*Xdot2[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot4[iThread+1] = dynamicModel->kuka_arm_dynamicsThread10(augXThreadNext.head(stateSize) - dt*Xdot3[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        XnextThread[iThread+1] = augXThreadNext.head(stateSize) - (dt/6)*(Xdot1[iThread+1] + 2*Xdot2[iThread+1] + 2*Xdot3[iThread+1] + Xdot4[iThread+1]);
    }else if(enable_euler_){
        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread1(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - dt*Xdot1[iThread];

        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread1(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        XnextThread[iThread+1] = augXThreadNext.head(stateSize) - dt*Xdot1[iThread+1];
    }
}

void UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread11(stateAug_t augXThread, stateAug_t augXThreadNext, double dt, unsigned int iThread){
    // (Two Sigma Points) Backwards 4^th order Runge-Kutta step from X_{k+1} to X_k
    if(enable_rk4_){

        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread11(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        Xdot2[iThread] = dynamicModel->kuka_arm_dynamicsThread11(augXThread.head(stateSize) - 0.5*dt*Xdot1[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot3[iThread] = dynamicModel->kuka_arm_dynamicsThread11(augXThread.head(stateSize) - 0.5*dt*Xdot2[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot4[iThread] = dynamicModel->kuka_arm_dynamicsThread11(augXThread.head(stateSize) - dt*Xdot3[iThread], augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - (dt/6)*(Xdot1[iThread] + 2*Xdot2[iThread] + 2*Xdot3[iThread] + Xdot4[iThread]);
        
        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread11(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        Xdot2[iThread+1] = dynamicModel->kuka_arm_dynamicsThread11(augXThreadNext.head(stateSize) - 0.5*dt*Xdot1[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot3[iThread+1] = dynamicModel->kuka_arm_dynamicsThread11(augXThreadNext.head(stateSize) - 0.5*dt*Xdot2[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot4[iThread+1] = dynamicModel->kuka_arm_dynamicsThread11(augXThreadNext.head(stateSize) - dt*Xdot3[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        XnextThread[iThread+1] = augXThreadNext.head(stateSize) - (dt/6)*(Xdot1[iThread+1] + 2*Xdot2[iThread+1] + 2*Xdot3[iThread+1] + Xdot4[iThread+1]);
    }else if(enable_euler_){
        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread1(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - dt*Xdot1[iThread];

        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread1(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        XnextThread[iThread+1] = augXThreadNext.head(stateSize) - dt*Xdot1[iThread+1];
    }
}

void UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread12(stateAug_t augXThread, stateAug_t augXThreadNext, double dt, unsigned int iThread){
    // (Two Sigma Points) Backwards 4^th order Runge-Kutta step from X_{k+1} to X_k
    if(enable_rk4_){

        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread12(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        Xdot2[iThread] = dynamicModel->kuka_arm_dynamicsThread12(augXThread.head(stateSize) - 0.5*dt*Xdot1[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot3[iThread] = dynamicModel->kuka_arm_dynamicsThread12(augXThread.head(stateSize) - 0.5*dt*Xdot2[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot4[iThread] = dynamicModel->kuka_arm_dynamicsThread12(augXThread.head(stateSize) - dt*Xdot3[iThread], augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - (dt/6)*(Xdot1[iThread] + 2*Xdot2[iThread] + 2*Xdot3[iThread] + Xdot4[iThread]);
        
        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread12(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        Xdot2[iThread+1] = dynamicModel->kuka_arm_dynamicsThread12(augXThreadNext.head(stateSize) - 0.5*dt*Xdot1[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot3[iThread+1] = dynamicModel->kuka_arm_dynamicsThread12(augXThreadNext.head(stateSize) - 0.5*dt*Xdot2[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot4[iThread+1] = dynamicModel->kuka_arm_dynamicsThread12(augXThreadNext.head(stateSize) - dt*Xdot3[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        XnextThread[iThread+1] = augXThreadNext.head(stateSize) - (dt/6)*(Xdot1[iThread+1] + 2*Xdot2[iThread+1] + 2*Xdot3[iThread+1] + Xdot4[iThread+1]);
    }else if(enable_euler_){
        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread1(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - dt*Xdot1[iThread];

        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread1(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        XnextThread[iThread+1] = augXThreadNext.head(stateSize) - dt*Xdot1[iThread+1];
    }
}

void UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread13(stateAug_t augXThread, stateAug_t augXThreadNext, double dt, unsigned int iThread){
    // (Two Sigma Points) Backwards 4^th order Runge-Kutta step from X_{k+1} to X_k
    if(enable_rk4_){

        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread13(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        Xdot2[iThread] = dynamicModel->kuka_arm_dynamicsThread13(augXThread.head(stateSize) - 0.5*dt*Xdot1[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot3[iThread] = dynamicModel->kuka_arm_dynamicsThread13(augXThread.head(stateSize) - 0.5*dt*Xdot2[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot4[iThread] = dynamicModel->kuka_arm_dynamicsThread13(augXThread.head(stateSize) - dt*Xdot3[iThread], augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - (dt/6)*(Xdot1[iThread] + 2*Xdot2[iThread] + 2*Xdot3[iThread] + Xdot4[iThread]);
        
        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread13(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        Xdot2[iThread+1] = dynamicModel->kuka_arm_dynamicsThread13(augXThreadNext.head(stateSize) - 0.5*dt*Xdot1[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot3[iThread+1] = dynamicModel->kuka_arm_dynamicsThread13(augXThreadNext.head(stateSize) - 0.5*dt*Xdot2[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot4[iThread+1] = dynamicModel->kuka_arm_dynamicsThread13(augXThreadNext.head(stateSize) - dt*Xdot3[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        XnextThread[iThread+1] = augXThreadNext.head(stateSize) - (dt/6)*(Xdot1[iThread+1] + 2*Xdot2[iThread+1] + 2*Xdot3[iThread+1] + Xdot4[iThread+1]);
    }else if(enable_euler_){
        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread1(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - dt*Xdot1[iThread];

        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread1(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        XnextThread[iThread+1] = augXThreadNext.head(stateSize) - dt*Xdot1[iThread+1];
    }
}

void UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread14(stateAug_t augXThread, stateAug_t augXThreadNext, double dt, unsigned int iThread){
    // (Two Sigma Points) Backwards 4^th order Runge-Kutta step from X_{k+1} to X_k
    if(enable_rk4_){

        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread14(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        Xdot2[iThread] = dynamicModel->kuka_arm_dynamicsThread14(augXThread.head(stateSize) - 0.5*dt*Xdot1[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot3[iThread] = dynamicModel->kuka_arm_dynamicsThread14(augXThread.head(stateSize) - 0.5*dt*Xdot2[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot4[iThread] = dynamicModel->kuka_arm_dynamicsThread14(augXThread.head(stateSize) - dt*Xdot3[iThread], augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - (dt/6)*(Xdot1[iThread] + 2*Xdot2[iThread] + 2*Xdot3[iThread] + Xdot4[iThread]);
        
        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread14(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        Xdot2[iThread+1] = dynamicModel->kuka_arm_dynamicsThread14(augXThreadNext.head(stateSize) - 0.5*dt*Xdot1[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot3[iThread+1] = dynamicModel->kuka_arm_dynamicsThread14(augXThreadNext.head(stateSize) - 0.5*dt*Xdot2[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot4[iThread+1] = dynamicModel->kuka_arm_dynamicsThread14(augXThreadNext.head(stateSize) - dt*Xdot3[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
    XnextThread[iThread+1] = augXThreadNext.head(stateSize) - (dt/6)*(Xdot1[iThread+1] + 2*Xdot2[iThread+1] + 2*Xdot3[iThread+1] + Xdot4[iThread+1]);
    }else if(enable_euler_){
        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread1(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - dt*Xdot1[iThread];

        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread1(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        XnextThread[iThread+1] = augXThreadNext.head(stateSize) - dt*Xdot1[iThread+1];
    }
}

void UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread15(stateAug_t augXThread, stateAug_t augXThreadNext, double dt, unsigned int iThread){
    // (Two Sigma Points) Backwards 4^th order Runge-Kutta step from X_{k+1} to X_k
    if(enable_rk4_){

        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread15(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        Xdot2[iThread] = dynamicModel->kuka_arm_dynamicsThread15(augXThread.head(stateSize) - 0.5*dt*Xdot1[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot3[iThread] = dynamicModel->kuka_arm_dynamicsThread15(augXThread.head(stateSize) - 0.5*dt*Xdot2[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot4[iThread] = dynamicModel->kuka_arm_dynamicsThread15(augXThread.head(stateSize) - dt*Xdot3[iThread], augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - (dt/6)*(Xdot1[iThread] + 2*Xdot2[iThread] + 2*Xdot3[iThread] + Xdot4[iThread]);
        
        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread15(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        Xdot2[iThread+1] = dynamicModel->kuka_arm_dynamicsThread15(augXThreadNext.head(stateSize) - 0.5*dt*Xdot1[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot3[iThread+1] = dynamicModel->kuka_arm_dynamicsThread15(augXThreadNext.head(stateSize) - 0.5*dt*Xdot2[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot4[iThread+1] = dynamicModel->kuka_arm_dynamicsThread15(augXThreadNext.head(stateSize) - dt*Xdot3[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        XnextThread[iThread+1] = augXThreadNext.head(stateSize) - (dt/6)*(Xdot1[iThread+1] + 2*Xdot2[iThread+1] + 2*Xdot3[iThread+1] + Xdot4[iThread+1]);
    }else if(enable_euler_){
        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread1(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - dt*Xdot1[iThread];

        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread1(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        XnextThread[iThread+1] = augXThreadNext.head(stateSize) - dt*Xdot1[iThread+1];
    }
}

void UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread16(stateAug_t augXThread, stateAug_t augXThreadNext, double dt, unsigned int iThread){
    // (Two Sigma Points) Backwards 4^th order Runge-Kutta step from X_{k+1} to X_k
    if(enable_rk4_){

        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread16(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        Xdot2[iThread] = dynamicModel->kuka_arm_dynamicsThread16(augXThread.head(stateSize) - 0.5*dt*Xdot1[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot3[iThread] = dynamicModel->kuka_arm_dynamicsThread16(augXThread.head(stateSize) - 0.5*dt*Xdot2[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot4[iThread] = dynamicModel->kuka_arm_dynamicsThread16(augXThread.head(stateSize) - dt*Xdot3[iThread], augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - (dt/6)*(Xdot1[iThread] + 2*Xdot2[iThread] + 2*Xdot3[iThread] + Xdot4[iThread]);
        
        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread16(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        Xdot2[iThread+1] = dynamicModel->kuka_arm_dynamicsThread16(augXThreadNext.head(stateSize) - 0.5*dt*Xdot1[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot3[iThread+1] = dynamicModel->kuka_arm_dynamicsThread16(augXThreadNext.head(stateSize) - 0.5*dt*Xdot2[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot4[iThread+1] = dynamicModel->kuka_arm_dynamicsThread16(augXThreadNext.head(stateSize) - dt*Xdot3[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        XnextThread[iThread+1] = augXThreadNext.head(stateSize) - (dt/6)*(Xdot1[iThread+1] + 2*Xdot2[iThread+1] + 2*Xdot3[iThread+1] + Xdot4[iThread+1]);
    }else if(enable_euler_){
        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread1(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - dt*Xdot1[iThread];

        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread1(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        XnextThread[iThread+1] = augXThreadNext.head(stateSize) - dt*Xdot1[iThread+1];
    }
}

void UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread17(stateAug_t augXThread, stateAug_t augXThreadNext, double dt, unsigned int iThread){
    // (Two Sigma Points) Backwards 4^th order Runge-Kutta step from X_{k+1} to X_k
    if(enable_rk4_){

        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread17(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        Xdot2[iThread] = dynamicModel->kuka_arm_dynamicsThread17(augXThread.head(stateSize) - 0.5*dt*Xdot1[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot3[iThread] = dynamicModel->kuka_arm_dynamicsThread17(augXThread.head(stateSize) - 0.5*dt*Xdot2[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot4[iThread] = dynamicModel->kuka_arm_dynamicsThread17(augXThread.head(stateSize) - dt*Xdot3[iThread], augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - (dt/6)*(Xdot1[iThread] + 2*Xdot2[iThread] + 2*Xdot3[iThread] + Xdot4[iThread]);
        
        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread17(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        Xdot2[iThread+1] = dynamicModel->kuka_arm_dynamicsThread17(augXThreadNext.head(stateSize) - 0.5*dt*Xdot1[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot3[iThread+1] = dynamicModel->kuka_arm_dynamicsThread17(augXThreadNext.head(stateSize) - 0.5*dt*Xdot2[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot4[iThread+1] = dynamicModel->kuka_arm_dynamicsThread17(augXThreadNext.head(stateSize) - dt*Xdot3[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        XnextThread[iThread+1] = augXThreadNext.head(stateSize) - (dt/6)*(Xdot1[iThread+1] + 2*Xdot2[iThread+1] + 2*Xdot3[iThread+1] + Xdot4[iThread+1]);
    }else if(enable_euler_){
        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread1(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - dt*Xdot1[iThread];

        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread1(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        XnextThread[iThread+1] = augXThreadNext.head(stateSize) - dt*Xdot1[iThread+1];
    }
}

void UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread18(stateAug_t augXThread, stateAug_t augXThreadNext, double dt, unsigned int iThread){
    // (Two Sigma Points) Backwards 4^th order Runge-Kutta step from X_{k+1} to X_k
    if(enable_rk4_){

        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread18(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        Xdot2[iThread] = dynamicModel->kuka_arm_dynamicsThread18(augXThread.head(stateSize) - 0.5*dt*Xdot1[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot3[iThread] = dynamicModel->kuka_arm_dynamicsThread18(augXThread.head(stateSize) - 0.5*dt*Xdot2[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot4[iThread] = dynamicModel->kuka_arm_dynamicsThread18(augXThread.head(stateSize) - dt*Xdot3[iThread], augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - (dt/6)*(Xdot1[iThread] + 2*Xdot2[iThread] + 2*Xdot3[iThread] + Xdot4[iThread]);
        
        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread18(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        Xdot2[iThread+1] = dynamicModel->kuka_arm_dynamicsThread18(augXThreadNext.head(stateSize) - 0.5*dt*Xdot1[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot3[iThread+1] = dynamicModel->kuka_arm_dynamicsThread18(augXThreadNext.head(stateSize) - 0.5*dt*Xdot2[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot4[iThread+1] = dynamicModel->kuka_arm_dynamicsThread18(augXThreadNext.head(stateSize) - dt*Xdot3[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        XnextThread[iThread+1] = augXThreadNext.head(stateSize) - (dt/6)*(Xdot1[iThread+1] + 2*Xdot2[iThread+1] + 2*Xdot3[iThread+1] + Xdot4[iThread+1]);
    }else if(enable_euler_){
        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread1(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - dt*Xdot1[iThread];

        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread1(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        XnextThread[iThread+1] = augXThreadNext.head(stateSize) - dt*Xdot1[iThread+1];
    }
}

void UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread19(stateAug_t augXThread, stateAug_t augXThreadNext, double dt, unsigned int iThread){
    // (Two Sigma Points) Backwards 4^th order Runge-Kutta step from X_{k+1} to X_k
    if(enable_rk4_){

        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread19(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        Xdot2[iThread] = dynamicModel->kuka_arm_dynamicsThread19(augXThread.head(stateSize) - 0.5*dt*Xdot1[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot3[iThread] = dynamicModel->kuka_arm_dynamicsThread19(augXThread.head(stateSize) - 0.5*dt*Xdot2[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot4[iThread] = dynamicModel->kuka_arm_dynamicsThread19(augXThread.head(stateSize) - dt*Xdot3[iThread], augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - (dt/6)*(Xdot1[iThread] + 2*Xdot2[iThread] + 2*Xdot3[iThread] + Xdot4[iThread]);
        
        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread19(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        Xdot2[iThread+1] = dynamicModel->kuka_arm_dynamicsThread19(augXThreadNext.head(stateSize) - 0.5*dt*Xdot1[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot3[iThread+1] = dynamicModel->kuka_arm_dynamicsThread19(augXThreadNext.head(stateSize) - 0.5*dt*Xdot2[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot4[iThread+1] = dynamicModel->kuka_arm_dynamicsThread19(augXThreadNext.head(stateSize) - dt*Xdot3[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        XnextThread[iThread+1] = augXThreadNext.head(stateSize) - (dt/6)*(Xdot1[iThread+1] + 2*Xdot2[iThread+1] + 2*Xdot3[iThread+1] + Xdot4[iThread+1]);
    }else if(enable_euler_){
        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread1(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - dt*Xdot1[iThread];

        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread1(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        XnextThread[iThread+1] = augXThreadNext.head(stateSize) - dt*Xdot1[iThread+1];
    }
}

void UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread20(stateAug_t augXThread, stateAug_t augXThreadNext, double dt, unsigned int iThread){
    // (Two Sigma Points) Backwards 4^th order Runge-Kutta step from X_{k+1} to X_k
    if(enable_rk4_){

        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread20(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        Xdot2[iThread] = dynamicModel->kuka_arm_dynamicsThread20(augXThread.head(stateSize) - 0.5*dt*Xdot1[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot3[iThread] = dynamicModel->kuka_arm_dynamicsThread20(augXThread.head(stateSize) - 0.5*dt*Xdot2[iThread], augXThread.tail(commandSize), iThread/2);
        Xdot4[iThread] = dynamicModel->kuka_arm_dynamicsThread20(augXThread.head(stateSize) - dt*Xdot3[iThread], augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - (dt/6)*(Xdot1[iThread] + 2*Xdot2[iThread] + 2*Xdot3[iThread] + Xdot4[iThread]);
        
        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread20(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        Xdot2[iThread+1] = dynamicModel->kuka_arm_dynamicsThread20(augXThreadNext.head(stateSize) - 0.5*dt*Xdot1[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot3[iThread+1] = dynamicModel->kuka_arm_dynamicsThread20(augXThreadNext.head(stateSize) - 0.5*dt*Xdot2[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        Xdot4[iThread+1] = dynamicModel->kuka_arm_dynamicsThread20(augXThreadNext.head(stateSize) - dt*Xdot3[iThread+1], augXThreadNext.tail(commandSize), iThread/2);
        XnextThread[iThread+1] = augXThreadNext.head(stateSize) - (dt/6)*(Xdot1[iThread+1] + 2*Xdot2[iThread+1] + 2*Xdot3[iThread+1] + Xdot4[iThread+1]);
    }else if(enable_euler_){
        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread1(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - dt*Xdot1[iThread];

        Xdot1[iThread+1] = dynamicModel->kuka_arm_dynamicsThread1(augXThreadNext.head(stateSize), augXThreadNext.tail(commandSize), iThread/2);
        XnextThread[iThread+1] = augXThreadNext.head(stateSize) - dt*Xdot1[iThread+1];
    }
}

void UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread21(stateAug_t augXThread, double dt, unsigned int iThread){
    // (Two Sigma Points) Backwards 4^th order Runge-Kutta step from X_{k+1} to X_k
    if(enable_rk4_){
        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread21(augXThread.head(stateSize), augXThread.tail(commandSize), 20);
        Xdot2[iThread] = dynamicModel->kuka_arm_dynamicsThread21(augXThread.head(stateSize) - 0.5*dt*Xdot1[iThread], augXThread.tail(commandSize), 20);
        Xdot3[iThread] = dynamicModel->kuka_arm_dynamicsThread21(augXThread.head(stateSize) - 0.5*dt*Xdot2[iThread], augXThread.tail(commandSize), 20);
        Xdot4[iThread] = dynamicModel->kuka_arm_dynamicsThread21(augXThread.head(stateSize) - dt*Xdot3[iThread], augXThread.tail(commandSize), 20);
        XnextThread[iThread] = augXThread.head(stateSize) - (dt/6)*(Xdot1[iThread] + 2*Xdot2[iThread] + 2*Xdot3[iThread] + Xdot4[iThread]);
        
    }else if(enable_euler_){
        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread1(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - dt*Xdot1[iThread];

    }
}

void UDPSolver::rungeKuttaStepBackwardTwoSigmaPointsThread22(stateAug_t augXThread, double dt, unsigned int iThread){
    // (Two Sigma Points) Backwards 4^th order Runge-Kutta step from X_{k+1} to X_k
    if(enable_rk4_){

        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread22(augXThread.head(stateSize), augXThread.tail(commandSize), 21);
        Xdot2[iThread] = dynamicModel->kuka_arm_dynamicsThread22(augXThread.head(stateSize) - 0.5*dt*Xdot1[iThread], augXThread.tail(commandSize), 21);
        Xdot3[iThread] = dynamicModel->kuka_arm_dynamicsThread22(augXThread.head(stateSize) - 0.5*dt*Xdot2[iThread], augXThread.tail(commandSize), 21);
        Xdot4[iThread] = dynamicModel->kuka_arm_dynamicsThread22(augXThread.head(stateSize) - dt*Xdot3[iThread], augXThread.tail(commandSize), 21);
        XnextThread[iThread] = augXThread.head(stateSize) - (dt/6)*(Xdot1[iThread] + 2*Xdot2[iThread] + 2*Xdot3[iThread] + Xdot4[iThread]);
        
    }else if(enable_euler_){
        Xdot1[iThread] = dynamicModel->kuka_arm_dynamicsThread1(augXThread.head(stateSize), augXThread.tail(commandSize), iThread/2);
        XnextThread[iThread] = augXThread.head(stateSize) - dt*Xdot1[iThread];
    }
}

stateVec_t UDPSolver::eulerStepBackward(stateAug_t augX, double& dt, unsigned int i){
    // Backwards Euler step from X_{k+1} to X_k
    Xdot1[i] = dynamicModel->kuka_arm_dynamics(augX.head(stateSize), augX.tail(commandSize));
    return augX.head(stateSize) - dt*Xdot1[i];
}

stateVec_t UDPSolver::rungeKutta3StepBackward(stateAug_t augX, commandVec_t U_previous, double& dt, unsigned int i){
    // Backwards Third-order step from X_{k+1} to X_k
    Xdot1[i] = dynamicModel->kuka_arm_dynamics(augX.head(stateSize), augX.tail(commandSize));
    Xdot2[i] = dynamicModel->kuka_arm_dynamics(augX.head(stateSize) - 0.5*dt*Xdot1[i], (augX.tail(commandSize) + U_previous)/2.0);
    Xdot3[i] = dynamicModel->kuka_arm_dynamics(augX.head(stateSize) + dt*Xdot1[i] - 2*dt*Xdot2[i], U_previous);
    return augX.head(stateSize) - (dt/6)*(Xdot1[i] + 4*Xdot2[i] + Xdot3[i]);
}

}  // namespace
}  // namespace kuka_iiwa_arm
}  // namespace examples
}  // namespace drake