/*
 ISC License

 Copyright (c) 2022, Autonomous Vehicle Systems Lab, University of Colorado at Boulder

 Permission to use, copy, modify, and/or distribute this software for any
 purpose with or without fee is hereby granted, provided that the above
 copyright notice and this permission notice appear in all copies.

 THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

 */

#include "spinningBodyStateEffector.h"
#include "architecture/utilities/avsEigenSupport.h"
#include "architecture/utilities/rigidBodyKinematics.h"
#include "architecture/utilities/avsEigenSupport.h"
#include "architecture/utilities/macroDefinitions.h"
#include <iostream>
#include <string>

/*! This is the constructor, setting variables to default values */
SpinningBodyStateEffector::SpinningBodyStateEffector()
{
    // Zero the mass props and mass prop rates contributions
    this->effProps.mEff = 0.0;
    this->effProps.rEff_CB_B.fill(0.0);
    this->effProps.IEffPntB_B.fill(0.0);
    this->effProps.rEffPrime_CB_B.fill(0.0);
    this->effProps.IEffPrimePntB_B.fill(0.0);

    // Initialize variables to working values
    this->mass = 0.0;
    this->thetaInit = 0.00;
    this->thetaDotInit = 0.0;
    this->IPntSc_S.Identity();
    this->dcm_BS.Identity();
    this->dcm_S0B.Identity();
    this->r_SB_B.setZero();
    this->sHat_S.setZero();
    this->u = 0.0;
    this->k = 1.0;
    this->c = 0.0;
    
    this->nameOfThetaState = "spinningBodyTheta" + std::to_string(this->effectorID);
    this->nameOfThetaDotState = "spinningBodyThetaDot" + std::to_string(this->effectorID);
    this->effectorID++; 

    return;
}

uint64_t SpinningBodyStateEffector::effectorID = 1;

/*! This is the destructor, nothing to report here */
SpinningBodyStateEffector::~SpinningBodyStateEffector()
{
    this->effectorID = 1;    /* reset the panel ID*/

    return;
}

/*! This method is used to reset the module. */
void SpinningBodyStateEffector::Reset(uint64_t CurrentClock)
{
    // Normalize the sHat vector (same in B or S frame components)
    if (this->sHat_S.norm() > 0.01) {
        this->sHat_S.normalize();
    }
    else {
        bskLogger.bskLog(BSK_ERROR, "Norm of sHat must be greater than 0. sHat may not have been set by the user.");
    }

    return;
}


/*! This method takes the computed theta states and outputs them to the messaging system. */
void SpinningBodyStateEffector::writeOutputStateMessages(uint64_t CurrentClock)
{
    // Write out the spinning body output messages
    if (this->spinningBodyOutMsg.isLinked()) {
        HingedRigidBodyMsgPayload spinningBodyBuffer;
        spinningBodyBuffer = this->spinningBodyOutMsg.zeroMsgPayload;
        spinningBodyBuffer.theta = this->theta;
        spinningBodyBuffer.thetaDot = this->thetaDot;
        this->spinningBodyOutMsg.write(&spinningBodyBuffer, this->moduleID, CurrentClock);
    }

    // Write out the spinning body state config log message
    if (this->spinningBodyConfigLogOutMsg.isLinked()) {
        SCStatesMsgPayload configLogMsg;
        configLogMsg = this->spinningBodyConfigLogOutMsg.zeroMsgPayload;

        // Logging the S frame is the body frame B of that object
        eigenVector3d2CArray(this->r_ScN_N, configLogMsg.r_BN_N);
        eigenVector3d2CArray(this->v_ScN_N, configLogMsg.v_BN_N);
        eigenVector3d2CArray(this->sigma_SN, configLogMsg.sigma_BN);
        eigenVector3d2CArray(this->omega_SN_S, configLogMsg.omega_BN_B);
        this->spinningBodyConfigLogOutMsg.write(&configLogMsg, this->moduleID, CurrentClock);
    }

}

/*! This method prepends the name of the spacecraft for multi-spacecraft simulations.*/
void SpinningBodyStateEffector::prependSpacecraftNameToStates()
{
    this->nameOfThetaState = this->nameOfSpacecraftAttachedTo + this->nameOfThetaState;
    this->nameOfThetaDotState = this->nameOfSpacecraftAttachedTo + this->nameOfThetaDotState;

    return;
}

/*! This method allows the SB state effector to have access to the hub states and gravity*/
void SpinningBodyStateEffector::linkInStates(DynParamManager& statesIn)
{
    // - Get access to the hub's sigma_BN, omegaBN_B and velocity needed for dynamic coupling and gravity
    std::string tmpMsgName;
    tmpMsgName = this->nameOfSpacecraftAttachedTo + "centerOfMassSC";
    this->c_B = statesIn.getPropertyReference(tmpMsgName);
    tmpMsgName = this->nameOfSpacecraftAttachedTo + "centerOfMassPrimeSC";
    this->cPrime_B = statesIn.getPropertyReference(tmpMsgName);

    this->hubSigma = statesIn.getStateObject(this->nameOfSpacecraftAttachedTo + "hubSigma");
    this->hubOmega = statesIn.getStateObject(this->nameOfSpacecraftAttachedTo + "hubOmega");
    this->hubPosition = statesIn.getStateObject(this->nameOfSpacecraftAttachedTo + "hubPosition");
    this->hubVelocity = statesIn.getStateObject(this->nameOfSpacecraftAttachedTo + "hubVelocity");

    return;
}

/*! This method allows the SB state effector to register its states: theta and thetaDot with the dynamic parameter manager */
void SpinningBodyStateEffector::registerStates(DynParamManager& states)
{
    // Register the theta state
    this->thetaState = states.registerState(1, 1, this->nameOfThetaState);
    Eigen::MatrixXd thetaInitMatrix(1,1);
    thetaInitMatrix(0,0) = this->thetaInit;
    this->thetaState->setState(thetaInitMatrix);

    // Register the thetaDot state
    this->thetaDotState = states.registerState(1, 1, this->nameOfThetaDotState);
    Eigen::MatrixXd thetaDotInitMatrix(1,1);
    thetaDotInitMatrix(0,0) = this->thetaDotInit;
    this->thetaDotState->setState(thetaDotInitMatrix);

    return;
}

/*! This method allows the SB state effector to provide its contributions to the mass props and mass prop rates of the
 spacecraft */
void SpinningBodyStateEffector::updateEffectorMassProps(double integTime)
{
    // Give the mass of the spinning body to the effProps mass
    this->effProps.mEff = this->mass;

    // Grab current states
    this->theta = this->thetaState->getState()(0, 0);
    this->thetaDot = this->thetaDotState->getState()(0, 0);

    // Compute the DCM from S frame to B frame and write sHat in B frame
    double dcm_S0S[3][3];
    double prv_S0S_array[3];
    Eigen::Vector3d prv_S0S = -this->theta * this->sHat_S;
    eigenVector3d2CArray(prv_S0S, prv_S0S_array);
    PRV2C(prv_S0S_array, dcm_S0S);
    this->dcm_BS = this->dcm_S0B.transpose() * c2DArray2EigenMatrix3d(dcm_S0S);
    this->sHat_B = this->dcm_BS * this->sHat_S;

    // Compute the effector's CoM with respect to point B
    this->r_ScS_B = this->dcm_BS * this->r_ScS_S;
    this->r_ScB_B = this->r_ScS_B + this->r_SB_B;
    this->effProps.rEff_CB_B = this->r_ScB_B;

    // Find the inertia of the hinged rigid body about point B
    this->rTilde_ScB_B = eigenTilde(this->r_ScB_B);
    this->IPntSc_B = this->dcm_BS * this->IPntSc_S * this->dcm_BS.transpose();
    this->effProps.IEffPntB_B = this->IPntSc_B - this->mass * this->rTilde_ScB_B * this->rTilde_ScB_B;

    // Define omega_SB_S and its cross product operator
    this->omega_SB_B = this->thetaDot * this->sHat_B;
    this->omegaTilde_SB_B = eigenTilde(this->omega_SB_B);

    // Find rPrime_ScB_B
    this->rPrime_ScS_B = this->omegaTilde_SB_B * this->r_ScS_B;
    this->rPrime_ScB_B = this->rPrime_ScS_B;
    this->effProps.rEffPrime_CB_B = this->rPrime_ScB_B;

    // Find body time derivative of IPntSc_B
    Eigen::Matrix3d rPrimeTilde_ScB_B = eigenTilde(this->rPrime_ScB_B);
    this->effProps.IEffPrimePntB_B = this->omegaTilde_SB_B* this->IPntSc_B - this->IPntSc_B *this->omegaTilde_SB_B
        - this->mass * (rPrimeTilde_ScB_B * this->rTilde_ScB_B + this->rTilde_ScB_B * rPrimeTilde_ScB_B);

    return;
}

/*! This method allows the SB state effector to give its contributions to the matrices needed for the back-sub 
 method */
void SpinningBodyStateEffector::updateContributions(double integTime, BackSubMatrices & backSubContr, Eigen::Vector3d sigma_BN, Eigen::Vector3d omega_BN_B, Eigen::Vector3d g_N)
{
    // Find the DCM from N to B frames
    this->sigma_BN = sigma_BN;
    this->dcm_BN = (this->sigma_BN.toRotationMatrix()).transpose();

    // Map gravity to body frame
    Eigen::Vector3d gLocal_N;
    Eigen::Vector3d g_B;
    gLocal_N = g_N;
    g_B = this->dcm_BN * gLocal_N;

    // Define omega_SN_B
    this->omega_BN_B = omega_BN_B;
    this->omegaTilde_BN_B = eigenTilde(this->omega_BN_B);
    this->omega_SN_B = this->omega_SB_B + this->omega_BN_B;
    Eigen::Matrix3d omegaTilde_SN_B = eigenTilde(this->omega_SN_B);

    // Define IPntS_B for compactness
    Eigen::Matrix3d rTilde_ScS_B = eigenTilde(this->r_ScS_B);
    Eigen::Matrix3d IPntS_B = this->IPntSc_B - this->mass * rTilde_ScS_B * rTilde_ScS_B;

    // Define auxiliary variable dTheta
    this->dTheta = this->sHat_B.transpose() * IPntS_B * this->sHat_B;

    // Define aTheta
    this->aTheta = this->mass * rTilde_ScS_B * this->sHat_B / this->dTheta;

    // Define bTheta
    Eigen::Matrix3d rTilde_SB_B = eigenTilde(this->r_SB_B);
    this->bTheta = -(IPntS_B - this->mass * rTilde_SB_B * rTilde_ScS_B) * this->sHat_B / this->dTheta;

    // Define cTheta with gravity gradient torque
    Eigen::Vector3d rDot_SB_B;
    Eigen::Vector3d gravityTorquePntS_B;
    rDot_SB_B = this->omegaTilde_BN_B * this->r_SB_B;
    gravityTorquePntS_B = rTilde_ScS_B * this->mass * g_B;
    this->cTheta = (this->sHat_B.dot(gravityTorquePntS_B - omegaTilde_SN_B * IPntS_B * this->omega_SN_B
        - IPntS_B * this->omegaTilde_BN_B * this->omega_SB_B -  this->mass * rTilde_ScS_B * this->omegaTilde_BN_B * rDot_SB_B) + this->u - this->k * this->theta - this->c * this->thetaDot) / this->dTheta;

    // For documentation on contributions see Vaz Carneiro, Allard, Schaub spinning body paper
    // Translation contributions
    backSubContr.matrixA = -this->mass * rTilde_ScS_B * this->sHat_B * this->aTheta.transpose();
    backSubContr.matrixB = -this->mass * rTilde_ScS_B * this->sHat_B * this->bTheta.transpose();
    backSubContr.vecTrans = -this->mass * this->omegaTilde_SB_B * this->rPrime_ScS_B + this->mass * rTilde_ScS_B * this->sHat_B * this->cTheta;

    // Rotation contributions
    backSubContr.matrixC = (this->IPntSc_B - this->mass * this->rTilde_ScB_B * rTilde_ScS_B) * this->sHat_B * this->aTheta.transpose();
    backSubContr.matrixD = (this->IPntSc_B - this->mass * this->rTilde_ScB_B * rTilde_ScS_B) * this->sHat_B * this->bTheta.transpose();
    backSubContr.vecRot = -omegaTilde_SN_B * this->IPntSc_B * this->omega_SB_B - this->mass * this->omegaTilde_BN_B * this->rTilde_ScB_B * this->rPrime_ScB_B
        - this->mass * this->rTilde_ScB_B * this->omegaTilde_SB_B * this->rPrime_ScS_B
        - (this->IPntSc_B - this->mass * this->rTilde_ScB_B * rTilde_ScS_B) * this->sHat_B * this->cTheta;

    return;
}

/*! This method is used to find the derivatives for the SB stateEffector: thetaDDot and the kinematic derivative */
void SpinningBodyStateEffector::computeDerivatives(double integTime, Eigen::Vector3d rDDot_BN_N, Eigen::Vector3d omegaDot_BN_B, Eigen::Vector3d sigma_BN)
{
    // Update dcm_BN
    this->sigma_BN = sigma_BN;
    this->dcm_BN = (this->sigma_BN.toRotationMatrix()).transpose();

    // Grab omegaDot_BN_B 
    Eigen::Vector3d omegaDotLocal_BN_B;
    omegaDotLocal_BN_B = omegaDot_BN_B;

    // Find rDDotLoc_BN_B
    Eigen::Vector3d rDDotLocal_BN_N = rDDot_BN_N;
    Eigen::Vector3d rDDotLocal_BN_B;
    rDDotLocal_BN_B = this->dcm_BN * rDDotLocal_BN_N;

    // Compute Derivatives
    this->thetaState->setDerivative(this->thetaDotState->getState());
    Eigen::MatrixXd thetaDDot(1, 1);
    thetaDDot(0, 0) = this->aTheta.dot(rDDotLocal_BN_B) + this->bTheta.dot(omegaDotLocal_BN_B) + this->cTheta;
    this->thetaDotState->setDerivative(thetaDDot);
    

    return;
}

/*! This method is for calculating the contributions of the SB state effector to the energy and momentum of the spacecraft */
void SpinningBodyStateEffector::updateEnergyMomContributions(double integTime, Eigen::Vector3d & rotAngMomPntCContr_B,
                                                                double & rotEnergyContr, Eigen::Vector3d omega_BN_B)
{
    // Update omega_BN_B and omega_SN_B
    this->omega_BN_B = omega_BN_B;
    this->omegaTilde_BN_B = eigenTilde(this->omega_BN_B);
    this->omega_SN_B = this->omega_SB_B + this->omega_BN_B;

    // Compute rDot_ScB_B
    this->rDot_ScB_B = this->rPrime_ScB_B + this->omegaTilde_BN_B * this->r_ScB_B;

    // Find rotational angular momentum contribution from hub
    rotAngMomPntCContr_B = this->IPntSc_B * this->omega_SN_B + this->mass * this->rTilde_ScB_B * this->rDot_ScB_B;

    // Find rotational energy contribution from the hub
    rotEnergyContr = 1.0 / 2.0 * this->omega_SN_B.dot(this->IPntSc_B * this->omega_SN_B) + 1.0 / 2.0 * this->mass * this->rDot_ScB_B.dot(this->rDot_ScB_B) + 1.0 / 2.0 * this->k * this->theta * this->theta;

    return;
}

/*! This method computes the spinning body states relative to the inertial frame */
void SpinningBodyStateEffector::computeSpinningBodyInertialStates()
{
    // inertial attitude
    Eigen::Matrix3d dcm_SN;
    dcm_SN = (this->dcm_BS).transpose() * this->dcm_BN;
    this->sigma_SN = eigenMRPd2Vector3d(eigenC2MRP(dcm_SN));

    // inertial position vector
    this->r_ScN_N = (Eigen::Vector3d)this->hubPosition->getState() + this->dcm_BN.transpose() * this->r_ScB_B;

    // inertial velocity vector
    this->v_ScN_N = (Eigen::Vector3d)this->hubVelocity->getState() + this->dcm_BN.transpose() * this->rDot_ScB_B;

    return;
}

/*! This method is used so that the simulation will ask SB to update messages */
void SpinningBodyStateEffector::UpdateState(uint64_t CurrentSimNanos)
{
    //! - Zero the command buffer and read the incoming command array
    if (this->motorTorqueInMsg.isLinked() && this->motorTorqueInMsg.isWritten()) {
        ArrayMotorTorqueMsgPayload incomingCmdBuffer;
        incomingCmdBuffer = this->motorTorqueInMsg();
        this->u = incomingCmdBuffer.motorTorque[0];
    }

    /* Compute spinning body inertial states */
    this->computeSpinningBodyInertialStates();

    this->writeOutputStateMessages(CurrentSimNanos);

    return;
}
