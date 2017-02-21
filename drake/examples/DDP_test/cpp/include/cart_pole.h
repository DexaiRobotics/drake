#ifndef CARTPOLE_H
#define CARTPOLE_H

#include "config.h"

#include <iostream>
#include "dynamicmodel.h"
#include <Eigen/Dense>

using namespace Eigen;

class CartPole : public DynamicModel
{
public:
    CartPole(double& mydt);
private:
protected:

    // attributes //
public:
private:
    double dt;
    //static const unsigned int stateNb=4;
    //static const unsigned int commandNb=1;
public:
    static const double mc;
    static const double mp;
    static const double l;
    static const double g;

    static const double k;
    static const double R;
    static const double Jm;
    static const double Jl;
    static const double fvm;
    static const double Cf0;
    static const double a;

    //commandVec_t lowerCommandBounds;
    //commandVec_t upperCommandBounds;
private:
    stateVec_t Xreal;
    stateMat_t Id;
    stateMat_t A;
    stateMat_t Ad;
    stateR_commandC_t B;
    stateR_commandC_t Bd;
    double A13atan;
    double A33atan;
    stateMat_t fxBase;
    stateR_commandC_t fuBase;

    stateMat_t QxxCont;
    commandMat_t QuuCont;
    commandR_stateC_t QuxCont;

    stateMat_half_t H;
    stateMat_half_t C;
    stateVec_half_t G;
    stateR_half_commandC_t Bu;
    
    stateVec_t Xdot1;
    stateVec_t Xdot2;
    stateVec_t Xdot3;
    stateVec_t Xdot4;
    
    stateMat_t A1;
    stateMat_t A2;
    stateMat_t A3;
    stateMat_t A4;
    stateMat_t IdentityMat;

    stateR_commandC_t B1;
    stateR_commandC_t B2;
    stateR_commandC_t B3;
    stateR_commandC_t B4;

    stateVec_t Xp;
    stateVec_t Xp1;
    stateVec_t Xp2;
    stateVec_t Xp3;
    stateVec_t Xp4;

    stateVec_t Xm;
    stateVec_t Xm1;
    stateVec_t Xm2;
    stateVec_t Xm3;
    stateVec_t Xm4;

protected:
    // methods //
public:
    stateVec_t cart_pole_dynamics(const stateVec_t& X, const commandVec_t& U);
    stateVec_t update(const int& nargout, const double& dt, const stateVec_t& X, const commandVec_t& U, stateMat_t& A, stateVec_t& B);
    void grad(const int& nargout, const double& dt, const stateVec_t& X, const commandVec_t& U, stateMat_t& A, stateVec_t& B);
    stateVec_t computeNextState(double& dt, const stateVec_t& X,const stateVec_t& Xdes, const commandVec_t &U);
    void computeAllModelDeriv(double& dt, const stateVec_t& X,const stateVec_t& Xdes, const commandVec_t &U);
    stateMat_t computeTensorContxx(const stateVec_t& nextVx);
    commandMat_t computeTensorContuu(const stateVec_t& nextVx);
    commandR_stateC_t computeTensorContux(const stateVec_t& nextVx);

private:
protected:
        // accessors //
public:

};

#endif // ROMEOSIMPLEACTUATOR_H
