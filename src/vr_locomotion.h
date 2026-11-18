static void UpdateLocomotion(const BridgeTracking *sample,int play,unsigned now){
    const BridgeInput *input=&sample->input;
    const BridgeHand *move=&input->hands[accessibility.switchSticks?1:0],*turn=&input->hands[accessibility.switchSticks?0:1];
    float dead=accessibility.deadzone*.01f,forward=0,right=0;
    int moving=0,sprint=0;
    int quiet=weaponWheel.quiet; /* Always clear the physical right stick used by the wheel. */
    if(play && (move->valid&BRIDGE_HAND_GRIP_VALID) && !(quiet && move==&input->hands[1])){
        float q[4],direction[3],ahead[3]={0,0,-1};const float *orientation=sample->views[0].orientation;
        const BridgeHand *off=&input->hands[leftHanded?1:0];
        if(accessibility.walkDirection && (off->valid&BRIDGE_HAND_AIM_VALID))orientation=off->aim.orientation;
        Multiply(referenceInverse,orientation,q);Rotate(q,ahead,direction);
        float length=hypotf(direction[0],direction[2]);
        if(length<.1f){direction[0]=0;direction[2]=-1;}else{direction[0]/=length;direction[2]/=length;}
        float x=move->stick[0],y=move->stick[1];
        if(hypotf(x,y)>=dead){
            forward=y*-direction[2]+x*-direction[0];right=y*direction[0]+x*-direction[2];
            moving=1;
            sprint=y>dead && (move->buttons&BRIDGE_BUTTON_STICK) && !(turn->buttons&BRIDGE_BUTTON_STICK);
        }
    }
    /* +speed inverts the native always-run preference. */
    SetControl(CONTROL_SPEED,moving && ((cvarFloat("cl_run")!=0)!=sprint));
    SetControl(1,forward>dead);SetControl(2,forward< -dead);SetControl(3,right< -dead);SetControl(4,right>dead);
    int available=play && (turn->valid&BRIDGE_HAND_GRIP_VALID) && !(quiet && turn==&input->hands[1]);
    float x=available?turn->stick[0]:0,y=available?turn->stick[1]:0;
    int vertical=available && fabsf(y)>fabsf(x) && fabsf(y)>fmaxf(dead,.7f),down=vertical && y<0;
    if(accessibility.crouchToggle && down && !crouchBefore)crouchLatched=!crouchLatched;
    crouchBefore=down;if(vertical && y>0)crouchLatched=0;
    if(!play)crouchLatched=crouchBefore=0;
    float height=(sample->views[0].position[1]+sample->views[1].position[1])*.5f;
    int physical=play && accessibility.physicalCrouch && !accessibility.seated && standingEyeHeight-height>.35f;
    SetControl(5,vertical && y>0);SetControl(6,play && (physical || (accessibility.crouchToggle?crouchLatched:down)));
    if(vertical)x=0;
    float dt=controlsTime?fminf(.05f,(now-controlsTime)*.001f):0;controlsTime=now;
    if(!available)turnLatched=1;
    else if(fabsf(x)<.25f)turnLatched=0;
    else if(!turnLatched){
        float delta=0;
        if(snapAngle && fabsf(x)>.7f){delta=x>0?-snapAngle:snapAngle;turnLatched=1;ResetKnifeMotion();}
        else if(!snapAngle && fabsf(x)>dead)delta=-copysignf((fabsf(x)-dead)/(1-dead),x)*accessibility.turnSpeed*dt;
        float *yaw=(float *)(gameBase+0x1f2c34);if(isfinite(*yaw))*yaw+=delta;
    }
}
