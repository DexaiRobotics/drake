function [xtraj,utraj,ctraj,btraj, psitraj,etatraj,jltraj, kltraj,straj, ...
    z,F,info,infeasible_constraint_name] = variationalTrajOptPeriodic()

options.floating = true;
options.ignore_self_collisions = true;
options.use_bullet = false;
options.terrain = RigidBodyFlatTerrain();
file = fullfile(getDrakePath,'examples','KneedCompassGait', 'KneedCompassGait.urdf');
plant = PlanarRigidBodyManipulator(file, options);  %RigidBodyManipulator(file,options);
v = plant.constructVisualizer();
v.inspector(zeros(12,1)); 

nq = plant.getNumPositions();
nv = plant.getNumVelocities();
nx = nv + nq; 
nu = plant.getNumInputs();

N = 21;
T0 = 2;

qo = [0    1 0 0 0 0]';
qm = [0 1 0.2 0 -0.4 0]';
q1 = [.1 1 0 0 0 0]';
xo = [qo; zeros(6,1)];
xm = [qm; zeros(6,1)];
x1 = [q1; zeros(6,1)];

t_init = linspace(0,T0,N);
traj_init.x = PPTrajectory(foh([0 T0],[xo x1]));
% traj_init.x = PPTrajectory(foh([0 T0/2 T0],[xo, xm, x1]));
traj_init.u = PPTrajectory(zoh(t_init,0.1*randn(3,N)));
T_span = [1 T0];


load_traj = 0; 
if load_traj
    load('TrajOpt2_07-Sep-2017')
    traj_init.x = xtraj; 
    traj_init.u = utraj; 
    traj_init.c = ctraj; 
    traj_init.b = btraj; 
    traj_init.psi = psitraj; 
    traj_init.eta = etatraj; 
    traj_init.s = straj; 
end

optimoptions.sweight = 10000;

traj_opt = VariationalTrajectoryOptimization(plant,N,T_span, optimoptions);
traj_opt = traj_opt.addRunningCost(@running_cost_fun);
traj_opt = traj_opt.addFinalCost(@final_cost_fun);
traj_opt = traj_opt.addTrajectoryDisplayFunction(@displayTraj);
traj_opt = traj_opt.addPositionConstraint(ConstantConstraint(qo),1);
% traj_opt = traj_opt.addPositionConstraint(ConstantConstraint(q1(1)),N, 1);
% traj_opt = traj_opt.addVelocityConstraint(ConstantConstraint(0*qo),1);

% -----Periodic Constraint-----

% Position 
Qperiodic = [-eye(nq-1), eye(nq-1)]; 
% Qperiodic(1,1) = 0; 
% Qperiodic(1, 1+nq) = 0; 
periodic_cnst_pos = LinearConstraint(zeros(nq-1,1),zeros(nq-1,1),Qperiodic);
periodic_cnst_pos = periodic_cnst_pos.setName('periodicity_pos');

% Inputs
% Rperiodic = [-eye(nu), eye(nu)]; 
% periodic_cnst_in = LinearConstraint(zeros(nu,1),zeros(nu,1),Rperiodic);
% periodic_cnst_in = periodic_cnst_in.setName('periodicity_input');

% Velocity 
cnstr_opts.grad_level = 1;
cnstr_opts.grad_method = 'user';
periodic_vars = 2 + 4*nq+2*nu+traj_opt.nC+traj_opt.nD*traj_opt.nC+2*traj_opt.nJL+2*traj_opt.nKL;
periodic_cnst_vel = FunctionHandleConstraint(zeros(nq,1), zeros(nq,1), periodic_vars, ...
    @periodic_constraint_fun, cnstr_opts);

periodic_cnst_vel = periodic_cnst_vel.setName('periodicity_vel');

periodic_inds = {traj_opt.h_inds(1); traj_opt.x_inds(:,1); traj_opt.x_inds(:,2); ...
    traj_opt.u_inds(:,1); traj_opt.c_inds(:,1); traj_opt.b_inds(:,1); traj_opt.jl_inds(:,1); ...
    traj_opt.kl_inds(:,1); traj_opt.h_inds(N-1); traj_opt.x_inds(:,N-1); traj_opt.x_inds(:,N); ...
    traj_opt.u_inds(:,N-1); traj_opt.kl_inds(:,N-1)};


traj_opt = traj_opt.addPositionConstraint(periodic_cnst_pos,{[1 N]}, [2:6]');
% traj_opt = traj_opt.addInputConstraint(periodic_cnst_in,{[1 N-1]});
traj_opt = traj_opt.addConstraint(periodic_cnst_vel, periodic_inds);

z_lb = qo(2) - 0.1; 
z_ub = qo(2) + 0.1;
traj_opt = traj_opt.addPositionConstraint(BoundingBoxConstraint(z_lb,z_ub),2:N-1, 2);

traj_opt = traj_opt.setSolver('snopt');
traj_opt = traj_opt.setSolverOptions('snopt','MajorIterationsLimit',10000);
traj_opt = traj_opt.setSolverOptions('snopt','MinorIterationsLimit',200000);
traj_opt = traj_opt.setSolverOptions('snopt','IterationsLimit',1000000);
traj_opt = traj_opt.setSolverOptions('snopt','SuperbasicsLimit',1000);


disp('Solving...')
tic
[xtraj,utraj,ctraj,btraj, psitraj,etatraj,jltraj, kltraj,straj, ...
    z,F,info,infeasible_constraint_name] = traj_opt.solveTraj(t_init,traj_init);
toc
%%

tt = xtraj.getBreaks();
h = tt(end)/N;
xx = xtraj.eval(tt);
qq = xx(1,:);
xx2 = xtraj.eval(tt + h/2);
vv = xx2(2,:);

uu = utraj.eval(tt + h/2);

figure(1); clf;
subplot(2,1,1); hold on;
plot(tt, rad2deg(qq));
plot(tt+h/2, rad2deg(vv));
subplot(2,1,2); hold on;
plot(tt+h/2, uu(1,:));

v.playback(xtraj, struct('slider', true'));

    function displayTraj(h,x,u)
        disp('Displaying Traj...');
        ts = [0;cumsum(h)];
        for i=1:length(ts)
            v.drawWrapper(0,x(:,i));
            pause(h(1)/10);
        end
    end

    function [f,df] = running_cost_fun(h,x,u)
        Q = 0* diag([0; 0; 0; 1; 0; 1; zeros(nv, 1)]); % penalize knee motions
        R = (1/50)^2*eye(nu);
        g = (1/2)*x'*Q*x + (1/2)*u'*R*u;
        f = h*g;
        df = [g, h*x'*Q, h*u'*R];
    end

    function [f,df] = final_cost_fun(tf,x)
        a = 0.3; 
        f = -a*x(1); 
%         f = (1/2)*[tf; x]'*[tf;x];
%         df = [tf; x]'; 
%         df = -eye(nx+1); 
        df = zeros(1, nx+1); 
        df(2) = -a;
    end

    function [f,df] = periodic_constraint_fun(h0,q0,q1,u0,c0,b0,jl0,kl0, ...
            hNm1,qNm1,qN,uNm1,klNm1)
        
        xin = [h0;q0;q1;u0;c0;b0;jl0;kl0; ...
            hNm1;qNm1;qN;uNm1;klNm1];
        [f,df] = periodic_constraint(xin);
        %         f
        %
%         df_fd = zeros(size(df));
%         step = 1e-6; % sqrt(eps(max(xin)));
%         dxin = step*eye(length(xin));
%         for k = 1:length(xin)
%             xin + dxin(:,k);
%             df_fd(:,k) = (periodic_constraint(xin+dxin(:,k)) - ...
%                 periodic_constraint(xin-dxin(:,k)))/(2*step);
%         end
%         
%         disp('Periodic constraint derivative error:');
%         disp(max(abs(df_fd(:)-df(:))));
    end

    function [f, df] = periodic_constraint(xin)
        
        nC = traj_opt.nC;
        nD = traj_opt.nD;
        nJL = traj_opt.nJL;
        nKL = traj_opt.nKL;
        nQ = traj_opt.plant.getNumPositions();
        nU = traj_opt.plant.getNumInputs();
        
        % left side
        h0 = xin(1);
        q0 = xin(1+(1:nQ));
        q1 = xin(1+nQ+(1:nQ));
        u0 = xin(1+2*nQ+(1:nU));
        c0 = xin(1+2*nQ+nU+(1:nC));
        b0 = xin(1+2*nQ+nU+nC+(1:nD*nC));
        jl0 = xin(1+2*nQ+nU+nC+nD*nC+(1:nJL));
        kl0 = xin(1+2*nQ+nU+nC+nD*nC+nJL+(1:nKL));
        
        % right side
        hNm1 = xin(1+2*nQ+nU+nC+nD*nC+nJL+1);
        qNm1 = xin(1+2*nQ+nU+nC+nD*nC+nJL+1+(1:nQ));
        qN = xin(1+2*nQ+nU+nC+nD*nC+nJL+1+nQ+(1:nQ));
        uNm1 = xin(1+2*nQ+nU+nC+nD*nC+nJL+1+2*nQ+(1:nU));
        klNm1 = xin(1+2*nQ+nU+nC+nD*nC+nJL+1+2*nQ+nU+(1:nKL));

        [p0, dp0] = left_legendre_transform_fun(traj_opt,h0,q0,q1,u0,c0,b0,jl0,kl0);
        [pN, dpN] = right_legendre_transform_fun(traj_opt,hNm1,qNm1,qN,uNm1,klNm1);
                
        f = pN - p0;
        
        df = zeros(nQ, numel(xin));
        
%         df(1:nQ, 1+(1:nQ)) = -eye(nQ);
%         df(1:nQ, 2+3*nQ+nU+nC+nC*nD+nJL+nKL+(1:nQ)) = eye(nQ);
        
        df(:, 1:1+2*nQ+nU+nC+nD*nC+nJL+nKL) = -dp0;
        df(:, 1+2*nQ+nU+nC+nD*nC+nJL+nKL+(1:1+2*nQ+nU+nKL)) = dpN;
        
%         df(2*nQ +1:2*nQ+nU, 1+2*nQ+(1:nU)) = -eye(nU);
%         df(2*nQ +1:2*nQ+nU, 2+4*nQ+nU+nC+nD*nC+nJL+(1:nU)) = eye(nU);
        
    end
end