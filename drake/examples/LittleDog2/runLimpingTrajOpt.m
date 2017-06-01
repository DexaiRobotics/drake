function [p,xtraj,utraj,ctraj,btraj,straj,z,F,info,traj_opt] = runLimpingTrajOpt()

load_saved = false;

options.terrain = RigidBodyFlatTerrain();
options.floating = true;
options.ignore_self_collisions = true;
options.use_bullet = false;
p = LittleDog(options);
v = p.constructVisualizer;

nq = p.getNumPositions();
nv = p.getNumVelocities();
nx = nq+nv;
nu = p.getNumInputs();

% Load nominal data
x0 = double(home(p));
q0 = x0(1:nq);
q0(3) = q0(3) - 0.010;

T0 = 4;
N = 30;

% ----- Initial Guess ----- %
q1 = [0.6;q0(2:end)];
x1 = [q1;zeros(nv,1)];

if load_saved
    load('limping_traj.mat');
    t_saved = linspace(xtraj.tspan(1),xtraj.tspan(2),length(xtraj.getBreaks));
    t_init = linspace(xtraj.tspan(1),xtraj.tspan(2),N);
    traj_init.x = xtraj;
    traj_init.u = utraj;
    traj_init.c = ctraj;
    traj_init.b = btraj;
    traj_init.eta = PPTrajectory(zoh(t_saved,[z(eta_inds),z(eta_inds(:,end))]));
    traj_init.psi = PPTrajectory(zoh(t_saved,[z(psi_inds),z(psi_inds(:,end))]));
    T_span = [1 T0];
else
    t_init = linspace(0,T0,N);
    % traj_init.x = PPTrajectory(foh([0 T0/2 T0],[x0, xm, x1]));
    traj_init.x = PPTrajectory(foh([0 T0],[x0, x1]));
    traj_init.u = PPTrajectory(zoh(t_init,0.1*randn(nu,N)));
    T_span = [1 T0];
end

traj_opt = VariationalTrajectoryOptimization(p,N,T_span);
traj_opt = traj_opt.addRunningCost(@running_cost_fun);
traj_opt = traj_opt.addNormalForceCost(@contact_cost_fun);
traj_opt = traj_opt.addPositionConstraint(ConstantConstraint(q0),1);  
% traj_opt = traj_opt.addPositionConstraint(ConstantConstraint(qm),7);
traj_opt = traj_opt.addPositionConstraint(ConstantConstraint(q1),N);
traj_opt = traj_opt.addVelocityConstraint(ConstantConstraint(zeros(nv,1)),1);

[q_lb, q_ub] = getJointLimits(p);
q_ub(3) = q0(3) + 0.01;
q_lb(3) = q0(3) - 0.02;
traj_opt = traj_opt.addPositionConstraint(BoundingBoxConstraint(q_lb,q_ub),2:N-1);


state_cost = Point(getStateFrame(p),ones(nx,1));
state_cost.base_x = 0;
state_cost.base_y = 0;
state_cost.base_pitch = 10;
state_cost.base_roll = 10;
state_cost.base_yaw = 10;
% state_cost.front_left_hip_roll = 5;
% state_cost.front_right_hip_roll = 5;
% state_cost.back_left_hip_roll = 5;
% state_cost.back_right_hip_roll = 5;
state_cost = double(state_cost);
Q = diag(state_cost); 


% traj_opt = traj_opt.addPositionConstraint(ConstantConstraint(qf(1:6)),N);
%traj_opt = traj_opt.addInputConstraint(ConstantConstraint(zeros(3,1)),1:N-1);
%traj_opt = traj_opt.addStateConstraint(ConstantConstraint(qm(1:6)),8);
% traj_opt = traj_opt.addStateConstraint(ConstantConstraint(qf(1:6)),N);
% traj_opt = traj_opt.addPositionConstraint(periodic_constraint,{[1 N]});

traj_opt = traj_opt.setSolverOptions('snopt','MajorIterationsLimit',10000);
traj_opt = traj_opt.setSolverOptions('snopt','MinorIterationsLimit',200000);
traj_opt = traj_opt.setSolverOptions('snopt','IterationsLimit',1000000);
traj_opt = traj_opt.setSolverOptions('snopt','SuperbasicsLimit',1000);

traj_opt = traj_opt.addTrajectoryDisplayFunction(@displayTraj);

tic
[xtraj,utraj,ctraj,btraj,straj,z,F,info,infeasible_constraint_name] = traj_opt.solveTraj(t_init,traj_init);
toc

v.playback(xtraj,struct('slider',true));

eta_inds = traj_opt.eta_inds;
psi_inds = traj_opt.psi_inds;
save('limping_traj','xtraj','utraj','ctraj','btraj','straj','eta_inds','psi_inds','z');

function [f,df] = running_cost_fun(h,x,u)
  R = 10*eye(nu);
  g = (1/2)*(x-x1)'*Q*(x-x1) + (1/2)*u'*R*u;
  f = h*g;
  df = [g, h*(x-x1)'*Q, h*u'*R];
end

function [f,df] = contact_cost_fun(c)
  W = diag([.1 0 0 0]);
  f = (1/2)*c'*W*c;
  df = c'*W;
end

  function displayTraj(h,x,u)
  
    ts = [0;cumsum(h)];
    for i=1:length(ts)
      v.drawWrapper(0,x(:,i));
      pause(h(1));
    end
   
end
end
