// -*- mode:C++; tab-width:4; c-basic-offset:4; indent-tabs-mode:nil -*-

/*
 * Copyright (C) 2015 iCub Facility - Istituto Italiano di Tecnologia
 * Author: Ugo Pattacini
 * CopyPolicy: Released under the terms of the LGPLv2.1 or later.
 *
 */

#define _USE_MATH_DEFINES
#include <cmath>
#include <string>
#include <algorithm>
#include <map>

#include <yarp/os/all.h>
#include <yarp/dev/all.h>
#include <yarp/sig/all.h>
#include <yarp/math/Math.h>

#include <hapticdevice/IHapticDevice.h>

#define DEG2RAD     (M_PI/180.0)
#define RAD2DEG     (180.0/M_PI)

using namespace std;
using namespace yarp::os;
using namespace yarp::dev;
using namespace yarp::sig;
using namespace yarp::math;
using namespace hapticdevice;


/**********************************************************/
class TeleOp: public RFModule
{
protected:
    PolyDriver     drvGeomagic;
    IHapticDevice *igeo;
    
    BufferedPort<Bottle> robotPort;
    BufferedPort<Bottle> simPort;

    string part;

    enum {
        idle,
        triggered,
        running
    };

    int s,c;
    bool no_torso;
    map<int,string> stateStr;

    Matrix Tsim;
    Vector pos0,rpy0;
    Vector x0,o0;

public:
    /**********************************************************/
    bool configure(ResourceFinder &rf)
    {
        string name=rf.check("name",Value("cer_teleop")).asString().c_str();
        string geomagic=rf.check("geomagic",Value("geomagic")).asString().c_str();
        double Tp2p=rf.check("Tp2p",Value(1.0)).asDouble();
        part=rf.check("part",Value("right_arm")).asString().c_str();

        Property optGeo("(device hapticdeviceclient)");
        optGeo.put("remote",("/"+geomagic).c_str());
        optGeo.put("local",("/"+name+"/geomagic").c_str());
        if (!drvGeomagic.open(optGeo))
            return false;
        drvGeomagic.view(igeo);

        s=idle; c=0;
        no_torso=true;
        
        stateStr[idle]="idle";
        stateStr[triggered]="triggered";
        stateStr[running]="running";

        Matrix T=zeros(4,4);
        T(0,1)=1.0;
        T(1,2)=1.0;
        T(2,0)=1.0;
        T(3,3)=1.0;
        igeo->setTransformation(SE3inv(T));
        
        pos0.resize(3,0.0);
        rpy0.resize(3,0.0);

        x0.resize(3,0.0);
        o0.resize(4,0.0);

        robotPort.open(("/"+name+"/target:o").c_str());
        simPort.open(("/"+name+"/sim:o").c_str());

        return true;
    }

    /**********************************************************/
    bool close()
    {
        igeo->setTransformation(eye(4,4));
        drvGeomagic.close();

        robotPort.close();
        simPort.close();

        return true;
    }

    /**********************************************************/
    void updateSim(const Vector &c_)
    {
        if ((c_.length()!=3) && (c_.length()!=4))
            return;

        Vector c=c_;        
        if (c.length()==3)
            c.push_back(1.0);
        c[3]=1.0;

        c=Tsim*c;

        Bottle cmd,reply;
        cmd.addString("world");
        cmd.addString("set");
        cmd.addString("ssph");

        // obj #
        cmd.addInt(1);

        // position
        cmd.addDouble(c[0]);
        cmd.addDouble(c[1]);
        cmd.addDouble(c[2]);

        simPort.write(cmd,reply);
    }

    /**********************************************************/
    void reachingHandler(const bool b, const Vector &pos,
                         const Vector &rpy)
    {
        if (b)
        {
            if (s==idle)
                s=triggered;
            else if (s==triggered)
            {
                if (++c*getPeriod()>0.5)
                {
                    pos0[0]=pos[0];
                    pos0[1]=pos[1];
                    pos0[2]=pos[2];

                    rpy0[0]=rpy[0];
                    rpy0[1]=rpy[1];
                    rpy0[2]=rpy[2];

                    iarm->getPose(x0,o0);
                    s=running;
                }
            }
            else
            {
                Vector xd(4,0.0);
                xd[0]=pos[0]-pos0[0];
                xd[1]=pos[1]-pos0[1];
                xd[2]=pos[2]-pos0[2];
                xd[3]=1.0;

                Matrix H0=eye(4,4);
                H0(0,3)=x0[0];
                H0(1,3)=x0[1];
                H0(2,3)=x0[2];

                xd=H0*xd;

                Matrix Rd;
                if (onlyXYZ)
                    Rd=axis2dcm(o0);
                else
                {
                    Vector drpy(3);
                    drpy[0]=rpy[0]-rpy0[0];
                    drpy[1]=rpy[1]-rpy0[1];
                    drpy[2]=rpy[2]-rpy0[2];

                    Vector ax(4,0.0),ay(4,0.0),az(4,0.0);
                    ax[0]=1.0; ax[3]=drpy[2];
                    ay[1]=1.0; ay[3]=drpy[1]*((part=="left_arm")?1.0:-1.0);
                    az[2]=1.0; az[3]=drpy[0]*((part=="left_arm")?1.0:-1.0);

                    Rd=axis2dcm(o0)*axis2dcm(ax)*axis2dcm(ay)*axis2dcm(az);
                }

                Vector od=dcm2axis(Rd);
                iarm->goToPose(xd,od);

                yInfo("going to (%s) (%s)",
                      xd.toString(3,3).c_str(),od.toString(3,3).c_str());

                updateSim(xd);
            }
        }
        else
        {
            if (s==triggered)
                no_torso=!no_torso;

            if (c!=0)
            {
                iarm->stopControl();
                if (simulator)
                {
                    Vector x,o;
                    iarm->getPose(x,o);
                    updateSim(x);
                }
            }

            s=idle;
            c=0;
        }
    }

    /**********************************************************/
    double getPeriod()
    {
        return 0.01;
    }

    /**********************************************************/
    bool updateModule()
    {
        Vector buttons,pos,rpy;
        igeo->getButtons(buttons);
        igeo->getPosition(pos);
        igeo->getOrientation(rpy);

        bool b0=(buttons[0]!=0.0);
        bool b1=(buttons[1]!=0.0);

        reachingHandler(b0,pos,rpy);
        yInfo("reaching=%s; torso=%s;",stateStr[s].c_str(),no_torso?"off":"on");

        return true;
    }
};


/**********************************************************/
int main(int argc,char *argv[])
{
    Network yarp;
    if (!yarp.checkNetwork())
    {
        yError("YARP server not found!");
        return 1;
    }

    ResourceFinder rf;
    rf.configure(argc,argv);

    TeleOp teleop;
    return teleop.runModule(rf);
}

