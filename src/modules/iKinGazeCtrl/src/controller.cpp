/* 
 * Copyright (C) 2010 RobotCub Consortium, European Commission FP6 Project IST-004370
 * Author: Ugo Pattacini
 * email:  ugo.pattacini@iit.it
 * website: www.robotcub.org
 * Permission is granted to copy, distribute, and/or modify this program
 * under the terms of the GNU General Public License, version 2 or any
 * later version published by the Free Software Foundation.
 *
 * A copy of the license can be found at
 * http://www.robotcub.org/icub/license/gpl.txt
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details
*/

#include <algorithm>

#include <iCub/solver.h>
#include <iCub/controller.h>


/************************************************************************/
Controller::Controller(PolyDriver *_drvTorso, PolyDriver *_drvHead, exchangeData *_commData,
                       const bool _posCtrlOn, const double _neckTime, const double _eyesTime,
                       const double _minAbsVel, const unsigned int _period) :
                       RateThread(_period), drvTorso(_drvTorso),   drvHead(_drvHead),
                       commData(_commData), posCtrlOn(_posCtrlOn), neckTime(_neckTime),
                       eyesTime(_eyesTime), minAbsVel(_minAbsVel), period(_period),
                       Ts(_period/1000.0),  printAccTime(0.0)
{
    Robotable=(drvHead!=NULL);

    // Instantiate objects
    neck=new iCubHeadCenter(commData->headV2?"right_v2":"right");
    eyeL=new iCubEye(commData->headV2?"left_v2":"left");
    eyeR=new iCubEye(commData->headV2?"right_v2":"right");

    // release links
    neck->releaseLink(0); eyeL->releaseLink(0); eyeR->releaseLink(0);
    neck->releaseLink(1); eyeL->releaseLink(1); eyeR->releaseLink(1);
    neck->releaseLink(2); eyeL->releaseLink(2); eyeR->releaseLink(2);

    // Get the chain objects
    chainNeck=neck->asChain();
    chainEyeL=eyeL->asChain();
    chainEyeR=eyeR->asChain();

    // add aligning matrices read from configuration file
    getAlignHN(commData->rf_cameras,"ALIGN_KIN_LEFT",eyeL->asChain());
    getAlignHN(commData->rf_cameras,"ALIGN_KIN_RIGHT",eyeR->asChain());

    // overwrite aligning matrices iff specified through tweak values
    if (commData->tweakOverwrite)
    {
        getAlignHN(commData->rf_tweak,"ALIGN_KIN_LEFT",eyeL->asChain());
        getAlignHN(commData->rf_tweak,"ALIGN_KIN_RIGHT",eyeR->asChain());
    }

    if (Robotable)
    {
        // read number of joints
        if (drvTorso!=NULL)
        {
            IEncoders *encTorso; drvTorso->view(encTorso);
            encTorso->getAxes(&nJointsTorso);
        }
        else
            nJointsTorso=3;
        
        IEncoders *encHead; drvHead->view(encHead);
        encHead->getAxes(&nJointsHead);

        drvHead->view(posHead);
        drvHead->view(velHead);

        // if requested check if position control is available
        if (posCtrlOn)
        {
            bool posDirAvailable=drvHead->view(posNeck); 
            bool vel2Available=drvHead->view(velEyes);
            posCtrlOn=posDirAvailable&&vel2Available;

            fprintf(stdout,"### neck control - requested POSITION mode: IPositionDirect [%s], IVelocityControl2 [%s] => %s mode selected\n",
                    posDirAvailable?"yes":"no",vel2Available?"yes":"no",posCtrlOn?"POSITION":"VELOCITY");
        }
        else
            fprintf(stdout,"### neck control - requested VELOCITY mode => VELOCITY mode selected\n");

        // joints bounds alignment
        lim=alignJointsBounds(chainNeck,drvTorso,drvHead,commData->eyeTiltMin,commData->eyeTiltMax);

        // read starting position
        fbTorso.resize(nJointsTorso,0.0);
        fbHead.resize(nJointsHead,0.0);

        // exclude acceleration constraints by fixing
        // thresholds at high values
        Vector a_robHead(nJointsHead,1e9);
        velHead->setRefAccelerations(a_robHead.data());
    }
    else
    {
        nJointsTorso=3;
        nJointsHead =6;

        // apply tilt bounds to eyes
        double min=std::max(CTRL_DEG2RAD*commData->eyeTiltMin,(*chainNeck)[nJointsTorso+3].getMin());
        double max=std::min(CTRL_DEG2RAD*commData->eyeTiltMax,(*chainNeck)[nJointsTorso+3].getMax());
        (*chainNeck)[nJointsTorso+3].setMin(min);
        (*chainNeck)[nJointsTorso+3].setMax(max);
        
        // create bounds matrix for integrators
        lim.resize(nJointsHead,2);
        for (int i=0; i<nJointsHead-1; i++)
        {
            lim(i,0)=(*chainNeck)[nJointsTorso+i].getMin();
            lim(i,1)=(*chainNeck)[nJointsTorso+i].getMax();
        }

        // vergence
        lim(nJointsHead-1,1)=lim(nJointsHead-2,1);

        fbTorso.resize(nJointsTorso,0.0);
        fbHead.resize(nJointsHead,0.0);
    }

    copyJointsBounds(chainNeck,chainEyeL);
    copyJointsBounds(chainEyeL,chainEyeR);

    // find minimum allowed vergence
    findMinimumAllowedVergence();

    // reinforce vergence min bound
    lim(nJointsHead-1,0)=commData->get_minAllowedVergence();

    if (Robotable)
        getFeedback(fbTorso,fbHead,drvTorso,drvHead,commData);
    else
        fbHead[5]=commData->get_minAllowedVergence();

    fbNeck=fbHead.subVector(0,2);
    fbEyes=fbHead.subVector(3,5);
    qdNeck.resize(3,0.0); qdEyes.resize(3,0.0);
    vNeck.resize(3,0.0);  vEyes.resize(3,0.0);

    // Set the task execution time
    setTeyes(eyesTime);
    setTneck(neckTime);

    mjCtrlNeck=new minJerkVelCtrlForIdealPlant(Ts,fbNeck.length());
    mjCtrlEyes=new minJerkVelCtrlForIdealPlant(Ts,fbEyes.length());
    IntState=new Integrator(Ts,fbHead,lim);
    IntPlan=new Integrator(Ts,fbNeck,lim.submatrix(0,2,0,1));
    
    v.resize(nJointsHead,0.0);

    neckJoints.resize(3);
    eyesJoints.resize(3);
    neckJoints[0]=0;
    neckJoints[1]=1;
    neckJoints[2]=2;
    eyesJoints[0]=3;
    eyesJoints[1]=4;
    eyesJoints[2]=5;

    qd=fbHead;
    q0deg=CTRL_RAD2DEG*qd;
    qddeg=q0deg;
    vdeg =CTRL_RAD2DEG*v;

    port_xd=NULL;
    ctrlActiveRisingEdgeTime=0.0;
    saccadeStartTime=0.0;
    tiltDone=panDone=verDone=false;
    unplugCtrlEyes=false;
}


/************************************************************************/
void Controller::findMinimumAllowedVergence()
{
    iKinChain cl(*chainEyeL), cr(*chainEyeR);
    Vector zeros(cl.getDOF(),0.0);
    cl.setAng(zeros); cr.setAng(zeros);

    double minVer=0.5*CTRL_DEG2RAD;
    double maxVer=lim(nJointsHead-1,1);
    for (double ver=0.0; ver<maxVer; ver+=0.5*CTRL_DEG2RAD)
    {
        cl(cl.getDOF()-1).setAng(ver/2.0);
        cr(cr.getDOF()-1).setAng(-ver/2.0);

        Vector fp(4);
        fp[3]=1.0;  // impose homogeneous coordinates
        if (computeFixationPointOnly(cl,cr,fp))
        {
            // if the component along eye's z-axis is positive
            // then this means that the fixation point is ok,
            // being in front of the robot
            Vector fpe=SE3inv(cl.getH())*fp;
            if (fpe[2]>0.0)
            {
                minVer=ver;
                break;
            }
        }
    }

    fprintf(stdout,"### computed minimum allowed vergence = %g [deg]\n",minVer*CTRL_RAD2DEG);
    commData->get_minAllowedVergence()=minVer;
}


/************************************************************************/
void Controller::minAllowedVergenceChanged()
{
    lim(nJointsHead-1,0)=commData->get_minAllowedVergence();
    IntState->setLim(lim);
}


/************************************************************************/
void Controller::notifyEvent(const string &event, const double checkPoint)
{
    if (port_event.getOutputCount()>0)
    {
        Bottle bottle;
        bottle.addString(event.c_str());
        bottle.addDouble(q_stamp);

        if (checkPoint>=0.0)
            bottle.addDouble(checkPoint);

        port_event.write(bottle);
    }
}


/************************************************************************/
void Controller::motionOngoingEventsHandling()
{
    if (motionOngoingEventsCurrent.size()!=0)
    {
        double curCheckPoint=*motionOngoingEventsCurrent.begin();
        double dist=norm(qddeg-q0deg);
        double checkPoint=(dist>ALMOST_ZERO)?norm(qdeg-q0deg)/dist:1.0;
        checkPoint=std::min(std::max(checkPoint,0.0),1.0);

        if (checkPoint>=curCheckPoint)
        {            
            notifyEvent("motion-ongoing",curCheckPoint);
            motionOngoingEventsCurrent.erase(curCheckPoint);
        }
    }
}


/************************************************************************/
void Controller::motionOngoingEventsFlush()
{
    while (motionOngoingEventsCurrent.size()!=0)
    {
        double curCheckPoint=*motionOngoingEventsCurrent.begin();
        notifyEvent("motion-ongoing",curCheckPoint);
        motionOngoingEventsCurrent.erase(curCheckPoint);
    }
}


/************************************************************************/
void Controller::stopLimb()
{
    if (Robotable)
    {
        // note: vel==0.0 is always achievable
        mutexCtrl.wait();

        if (posCtrlOn)
        {
            for (size_t i=0; i<neckJoints.size(); i++)
                posHead->stop(neckJoints[i]);

            for (size_t i=0; i<eyesJoints.size(); i++)
                velEyes->velocityMove(eyesJoints[i],0.0);
        }
        else for (int i=0; i<nJointsHead; i++)
            velHead->velocityMove(i,0.0);

        mutexCtrl.post();
    }
}


/************************************************************************/
void Controller::printIter(Vector &xd, Vector &fp, Vector &qd, Vector &q,
                           Vector &v, double printTime)
{
    if ((printAccTime+=Ts)>=printTime)
    {
        printAccTime=0.0;

        fprintf(stdout,"\n");
        fprintf(stdout,"norm(e)           = %g\n",norm(xd-fp));
        fprintf(stdout,"Target fix. point = %s\n",xd.toString().c_str());
        fprintf(stdout,"Actual fix. point = %s\n",fp.toString().c_str());
        fprintf(stdout,"Target Joints     = %s\n",qd.toString().c_str());
        fprintf(stdout,"Actual Joints     = %s\n",q.toString().c_str());
        fprintf(stdout,"Velocity          = %s\n",v.toString().c_str());
        fprintf(stdout,"\n");
    }
}


/************************************************************************/
bool Controller::threadInit()
{
    port_x.open((commData->localStemName+"/x:o").c_str());
    port_q.open((commData->localStemName+"/q:o").c_str());
    port_event.open((commData->localStemName+"/events:o").c_str());

    fprintf(stdout,"Starting Controller at %d ms\n",period);
    q_stamp=Time::now();

    return true;
}


/************************************************************************/
void Controller::afterStart(bool s)
{
    if (s)
        fprintf(stdout,"Controller started successfully\n");
    else
        fprintf(stdout,"Controller did not start\n");
}


/************************************************************************/
void Controller::doSaccade(Vector &ang, Vector &vel)
{
    mutexCtrl.wait();

    posHead->setRefSpeed(eyesJoints[0],vel[0]);
    posHead->setRefSpeed(eyesJoints[1],vel[1]);
    posHead->setRefSpeed(eyesJoints[2],vel[2]);

    // enforce joints bounds
    ang[0]=std::min(std::max(lim(eyesJoints[0],0),ang[0]),lim(eyesJoints[0],1));
    ang[1]=std::min(std::max(lim(eyesJoints[1],0),ang[1]),lim(eyesJoints[1],1));
    ang[2]=std::min(std::max(lim(eyesJoints[2],0),ang[2]),lim(eyesJoints[2],1));

    posHead->positionMove(eyesJoints[0],CTRL_RAD2DEG*ang[0]);
    posHead->positionMove(eyesJoints[1],CTRL_RAD2DEG*ang[1]);
    posHead->positionMove(eyesJoints[2],CTRL_RAD2DEG*ang[2]);

    saccadeStartTime=Time::now();
    tiltDone=panDone=verDone=false;
    commData->get_isSaccadeUnderway()=true;
    unplugCtrlEyes=true;

    notifyEvent("saccade-onset");

    mutexCtrl.post();    
}


/************************************************************************/
void Controller::resetCtrlEyes()
{
    mutexCtrl.wait();
    mjCtrlEyes->reset(zeros(3));
    unplugCtrlEyes=false;
    mutexCtrl.post();
}


/************************************************************************/
void Controller::run()
{
    string event="none";

    // verify if any saccade is still underway
    mutexCtrl.wait();
    if (commData->get_isSaccadeUnderway() && (Time::now()-saccadeStartTime>=Ts))
    {
        if (!tiltDone)
            posHead->checkMotionDone(eyesJoints[0],&tiltDone);

        if (!panDone)
            posHead->checkMotionDone(eyesJoints[1],&panDone);

        if (!verDone)
            posHead->checkMotionDone(eyesJoints[2],&verDone);

        commData->get_isSaccadeUnderway()=!(tiltDone&&panDone&&verDone);
        if (!commData->get_isSaccadeUnderway())
            notifyEvent("saccade-done");
    }
    mutexCtrl.post();
    
    // get data
    double x_stamp;
    Vector xd=commData->get_xd();
    Vector x=commData->get_x(x_stamp);
    Vector new_qd=commData->get_qd();

    // read feedbacks
    q_stamp=Time::now();
    if (Robotable)
    {
        if (!getFeedback(fbTorso,fbHead,drvTorso,drvHead,commData,&q_stamp))
        {
            fprintf(stdout,"\nCommunication timeout detected!\n\n");
            notifyEvent("comm-timeout");
            suspend();

            return;
        }

        IntState->reset(fbHead);
    }

    fbNeck=fbHead.subVector(0,2);
    fbEyes=fbHead.subVector(3,5);

    double errNeck=norm(new_qd.subVector(0,2)-fbNeck);
    double errEyes=norm(new_qd.subVector(3,new_qd.length()-1)-fbEyes);
    bool swOffCond=(Time::now()-ctrlActiveRisingEdgeTime<GAZECTRL_SWOFFCOND_DISABLETIME) ? false :
                   (!commData->get_isSaccadeUnderway() &&
                   (errNeck<GAZECTRL_MOTIONDONE_NECK_QTHRES*CTRL_DEG2RAD) &&
                   (errEyes<GAZECTRL_MOTIONDONE_EYES_QTHRES*CTRL_DEG2RAD));

    // verify control switching conditions
    if (commData->get_isCtrlActive())
    {
        // switch-off condition
        if (swOffCond)
        {
            event="motion-done";

            stopLimb();
            commData->get_isCtrlActive()=false;
        }
        // manage new target while controller is active
        else if (port_xd->get_new())
        {
            event="motion-onset";

            mutexData.wait();
            motionOngoingEventsCurrent=motionOngoingEvents;
            mutexData.post();
        }

        port_xd->get_new()=false;
    }
    else
    {
        // switch-on condition
        commData->get_isCtrlActive()=port_xd->get_new() || (new_qd[0]!=qd[0]) || (new_qd[1]!=qd[1]) || (new_qd[2]!=qd[2]) ||
                                     (!commData->get_canCtrlBeDisabled() && (norm(port_xd->get_xd()-x)>GAZECTRL_MOTIONSTART_XTHRES));

        // reset controllers
        if (commData->get_isCtrlActive())
        {
            ctrlActiveRisingEdgeTime=Time::now();
            port_xd->get_new()=false;

            Vector zeros(3,0.0);
            mjCtrlNeck->reset(zeros);
            mjCtrlEyes->reset(zeros);
            IntPlan->reset(fbNeck);

            event="motion-onset";

            mutexData.wait();
            motionOngoingEventsCurrent=motionOngoingEvents;
            mutexData.post();
        }
    }

    if (event=="motion-onset")
        q0deg=CTRL_RAD2DEG*fbHead;

    qd=new_qd;
    qdNeck=qd.subVector(0,2);
    qdEyes=qd.subVector(3,5);

    vNeck=0.0; vEyes=0.0;
    if (commData->get_isCtrlActive())
    {
        // control loop
        vNeck=mjCtrlNeck->computeCmd(neckTime,qdNeck-fbNeck);
        IntPlan->integrate(vNeck);

        if (unplugCtrlEyes)
        {
            if (Time::now()-saccadeStartTime>=Ts)
                vEyes=commData->get_counterv();
        }
        else
            vEyes=mjCtrlEyes->computeCmd(eyesTime,qdEyes-fbEyes)+commData->get_counterv();
    }

    v.setSubvector(0,vNeck);
    v.setSubvector(3,vEyes);

    // apply bang-bang just in case to compensate
    // for unachievable low velocities
    if (Robotable)
    {
        for (size_t i=0; i<v.length(); i++)
            if ((v[i]!=0.0) && (v[i]>-minAbsVel) && (v[i]<minAbsVel))
                v[i]=iCub::ctrl::sign(qd[i]-fbHead[i])*minAbsVel;
    }

    // convert to degrees
    mutexData.wait();
    qddeg=CTRL_RAD2DEG*qd;
    qdeg =CTRL_RAD2DEG*fbHead;
    vdeg =CTRL_RAD2DEG*v;
    mutexData.post();

    // send commands to the robot
    if (Robotable)
    {
        mutexCtrl.wait();

        if (posCtrlOn)
        {
            Vector posdeg=(CTRL_RAD2DEG)*IntPlan->get();
            posNeck->setPositions(neckJoints.size(),neckJoints.getFirst(),posdeg.data());
            Time::delay(0.001);     // workaround
            velEyes->velocityMove(eyesJoints.size(),eyesJoints.getFirst(),vdeg.subVector(3,5).data());
        }
        else
            velHead->velocityMove(vdeg.data());

        mutexCtrl.post();
    }

    // print info
    printIter(xd,x,qddeg,qdeg,vdeg,1.0);

    // send x,q through YARP ports
    Vector q(nJointsTorso+nJointsHead);
    int j;
    for (j=0; j<nJointsTorso; j++)
        q[j]=CTRL_RAD2DEG*fbTorso[j];
    for (; (size_t)j<q.length(); j++)
        q[j]=qdeg[j-nJointsTorso];

    txInfo_x.update(x_stamp);
    if (port_x.getOutputCount()>0)
    {        
        port_x.setEnvelope(txInfo_x);
        port_x.write(x);
    }

    txInfo_q.update(q_stamp);
    if (port_q.getOutputCount()>0)
    {        
        port_q.setEnvelope(txInfo_q);
        port_q.write(q);
    }

    // update pose information
    mutexChain.wait();

    for (int i=0; i<nJointsTorso; i++)
    {
        chainNeck->setAng(i,fbTorso[i]);
        chainEyeL->setAng(i,fbTorso[i]);
        chainEyeR->setAng(i,fbTorso[i]);
    }
    for (int i=0; i<3; i++)
    {
        chainNeck->setAng(nJointsTorso+i,fbHead[i]);
        chainEyeL->setAng(nJointsTorso+i,fbHead[i]);
        chainEyeR->setAng(nJointsTorso+i,fbHead[i]);
    }
    chainEyeL->setAng(nJointsTorso+3,fbHead[3]);               chainEyeR->setAng(nJointsTorso+3,fbHead[3]);
    chainEyeL->setAng(nJointsTorso+4,fbHead[4]+fbHead[5]/2.0); chainEyeR->setAng(nJointsTorso+4,fbHead[4]-fbHead[5]/2.0);

    txInfo_pose.update(q_stamp);

    mutexChain.post();

    if (event=="motion-onset")
        notifyEvent(event);

    motionOngoingEventsHandling();

    if (event=="motion-done")
    {
        motionOngoingEventsFlush();
        notifyEvent(event);
    }

    // update joints angles
    fbHead=IntState->integrate(v);
    commData->set_q(fbHead);
    commData->set_torso(fbTorso);
    commData->set_v(v);
}


/************************************************************************/
void Controller::threadRelease()
{
    stopLimb();
    notifyEvent("closing");

    port_x.interrupt();
    port_q.interrupt();
    port_event.interrupt();

    port_x.close();
    port_q.close();
    port_event.close();

    delete neck;
    delete eyeL;
    delete eyeR;
    delete mjCtrlNeck;
    delete mjCtrlEyes;
    delete IntState;
    delete IntPlan;
}


/************************************************************************/
void Controller::suspend()
{
    stopLimb();
    commData->get_isCtrlActive()=false;
    commData->get_isSaccadeUnderway()=false;
    fprintf(stdout,"\nController has been suspended!\n\n");
    RateThread::suspend();
    notifyEvent("suspended");
}


/************************************************************************/
void Controller::resume()
{
    if (Robotable)
    {
        getFeedback(fbTorso,fbHead,drvTorso,drvHead,commData);
        fbNeck=fbHead.subVector(0,2);
        fbEyes=fbHead.subVector(3,5);
    }

    fprintf(stdout,"\nController has been resumed!\n\n");
    RateThread::resume();
    notifyEvent("resumed");
}


/************************************************************************/
double Controller::getTneck() const
{
    return neckTime;
}


/************************************************************************/
double Controller::getTeyes() const
{
    return eyesTime;
}


/************************************************************************/
void Controller::setTneck(const double execTime)
{
    double lowerThresNeck=eyesTime+0.2;
    if (execTime<lowerThresNeck)
    {        
        fprintf(stdout,"Warning: neck execution time is under the lower bound!\n");
        fprintf(stdout,"A new neck execution time of %g s is chosen\n",lowerThresNeck);

        neckTime=lowerThresNeck;
    }
    else
        neckTime=execTime;
}


/************************************************************************/
void Controller::setTeyes(const double execTime)
{
    double lowerThresEyes=10.0*Ts;
    if (execTime<lowerThresEyes)
    {        
        fprintf(stdout,"Warning: eyes execution time is under the lower bound!\n");
        fprintf(stdout,"A new eyes execution time of %g s is chosen\n",lowerThresEyes);

        eyesTime=lowerThresEyes;
    }
    else
        eyesTime=execTime;
}


/************************************************************************/
bool Controller::isMotionDone() const
{
    return !commData->get_isCtrlActive();
}


/************************************************************************/
void Controller::setTrackingMode(const bool f)
{
    commData->get_canCtrlBeDisabled()=!f;

    if ((port_xd!=NULL) && f)
        port_xd->set_xd(commData->get_x());
}


/************************************************************************/
bool Controller::getTrackingMode() const 
{
    return !commData->get_canCtrlBeDisabled();
}


/************************************************************************/
bool Controller::getDesired(Vector &des)
{
    mutexData.wait();
    des=qddeg;
    mutexData.post();
    return true;
}


/************************************************************************/
bool Controller::getVelocity(Vector &vel)
{
    mutexData.wait();
    vel=vdeg;
    mutexData.post();
    return true;
}


/************************************************************************/
bool Controller::getPose(const string &poseSel, Vector &x, Stamp &stamp)
{
    if (poseSel=="left")
    {
        mutexChain.wait();
        x=chainEyeL->EndEffPose();
        stamp=txInfo_pose;
        mutexChain.post();
        return true;
    }
    else if (poseSel=="right")
    {
        mutexChain.wait();
        x=chainEyeR->EndEffPose();
        stamp=txInfo_pose;
        mutexChain.post();
        return true;
    }
    else if (poseSel=="head")
    {
        mutexChain.wait();
        x=chainNeck->EndEffPose();
        stamp=txInfo_pose;
        mutexChain.post();
        return true;
    }
    else
        return false;
}


/************************************************************************/
bool Controller::registerMotionOngoingEvent(const double checkPoint)
{
    if ((checkPoint>=0.0) && (checkPoint<=1.0))
    {
        mutexData.wait();
        motionOngoingEvents.insert(checkPoint);
        mutexData.post();

        return true;
    }
    else
        return false;
}


/************************************************************************/
bool Controller::unregisterMotionOngoingEvent(const double checkPoint)
{
    bool ret=false;
    if ((checkPoint>=0.0) && (checkPoint<=1.0))
    {
        mutexData.wait();
        multiset<double>::iterator itr=motionOngoingEvents.find(checkPoint);
        if (itr!=motionOngoingEvents.end())
        {
            motionOngoingEvents.erase(itr);
            ret=true;
        }
        mutexData.post();
    }

    return ret;
}


/************************************************************************/
Bottle Controller::listMotionOngoingEvents()
{
    Bottle events;

    mutexData.wait();
    for (multiset<double>::iterator itr=motionOngoingEvents.begin(); itr!=motionOngoingEvents.end(); itr++)
        events.addDouble(*itr);
    mutexData.post();

    return events;
}


