classdef KukaArm < TimeSteppingRigidBodyManipulator

  properties
    hand_name = 'iiwa_link_ee';
    disturbance_type = 1; % 1 ee-force, 2-state error, 3-torque
    brick_id
    left_finger_id
    right_finger_id
  end
  
  methods
  
    function obj = KukaArm(options)
      if nargin < 1
        options = struct();
      end
      
      if ~isfield(options,'floating')
        options.floating = false;
      end
      if ~isfield(options,'urdf')
        options.urdf = 'urdf/iiwa14_fixed_gripper.urdf';
      end
      if ~isfield(options,'with_weight')
        options.with_weight = false;
      end
      if ~isfield(options,'with_box')
        options.with_box = false;
      end
      if ~isfield(options,'with_shelf')
        options.with_shelf = false;
      end
      if ~isfield(options,'with_shelf_and_boxes')
        options.with_shelf_and_boxes = false;
      end
      if options.with_weight
        options.urdf = 'urdf/iiwa14_fixed_gripper.urdf';
      end

      warning('off','Drake:RigidBodyManipulator:UnsupportedVelocityLimits');
      warning('off','Drake:RigidBodyManipulator:UnsupportedContactPoints');
      warning('off','Drake:RigidBodyManipulator:WeldedLinkInd');
      warning('off','Drake:RigidBodyManipulator:UnsupportedJointLimits');
      obj = obj@TimeSteppingRigidBodyManipulator(options.urdf,0.001,options);

      if options.with_weight
        options_hand.weld_to_link = findLinkId(obj,obj.hand_name);
        obj = obj.addRobotFromURDF('urdf/robotiq_simple.urdf', [0;0;0.099], [pi/2;0;0], options_hand);
      end
      if options.with_box
        obj = obj.addRobotFromURDF('urdf/box.urdf', [0.6;0;1.4], [0;0;0]);
      end
      if options.with_shelf
        obj = obj.addRobotFromURDF('urdf/shelf.urdf', [0.6;0;0.88], [0;0;0]);
        obj = obj.addRobotFromURDF('urdf/shelf.urdf', [0.6;0.2;1.08], [pi/2;0;0]);
        obj = obj.addRobotFromURDF('urdf/shelf.urdf', [0.8;0.0;1.08], [0;pi/2;0]);
      end
      if options.with_shelf_and_boxes
        %obj = obj.addRobotFromURDF('urdf/box.urdf', [-0.3;-.9;.9], [0;0;0]);
        obj = obj.addRobotFromURDF('urdf/box.urdf', [-0.3;0;1.5], [0;0;0]);
        obj = obj.addRobotFromURDF('urdf/shelf.urdf', [0.6;0;0.88], [0;0;0]);
        obj = obj.addRobotFromURDF('urdf/shelf.urdf', [0.6;0.2;1.08], [pi/2;0;0]);
        obj = obj.addRobotFromURDF('urdf/shelf.urdf', [0.8;0.0;1.08], [0;pi/2;0]);
      end
      
      obj = obj.removeCollisionGroupsExcept({'manip'});
      options.floating = true;
      obj = obj.addRobotFromURDF('urdf/box.urdf',[],[],options);

      obj.brick_id = obj.findLinkId('brick');
      obj.left_finger_id = obj.findLinkId('left_finger');
      obj.right_finger_id = obj.findLinkId('iiwa_link_7+iiwa_link_ee+base_link+right_finger+iiwa_link_ee_kuka');
      
      obj = compile(obj);

    end
    
    function nw = getNumDisturbances(obj)
        switch obj.disturbance_type
            case 1
                nw = 3;
            case 2
                nw = obj.getNumContStates();
            case 3
                nw = obj.getNumInputs();
            otherwise
                error('Unknown disturbance type');
        end
    end
    
    function [f,df,d2f] = dynamics_w(obj,t,x,u,w)
        
        [f,df] = dynamics_w_ee(obj,t,x,u,w);
        
        if nargout == 3
            %Finite diff to get 2nd derivatives
            nx = length(x);
            nu = length(u);
            nw = length(w);
            
            Dx = 1e-6*eye(nx);
            Du = 1e-6*eye(nu);
            Dw = 1e-6*eye(nw);
            
            d2f = zeros(nx, 1+nx+nu+nw, 1+nx+nu+nw);
            for k = 1:nx
                [~,df_p] = dynamics_w_ee(obj,t,x+Dx(:,k),u,w);
                d2f(:,:,1+k) = df_p-df;
            end
            for k = 1:nu
                [~,df_p] = dynamics_w_ee(obj,t,x,u+Du(:,k),w);
                d2f(:,:,1+nx+k) = df_p-df;
            end
            for k = 1:nw
                [~,df_p] = dynamics_w_ee(obj,t,x,u,w+Dw(:,k));
                d2f(:,:,1+nx+nu+k) = df_p-df;
            end
            
            d2f = reshape(d2f,nx,(1+nx+nu+nw)*(1+nx+nu+nw));
            
        end

    end
    
    function [f,df] = dynamics_w_ee(obj,t,x,u,w)
      % w should be a 3x1 force vector in world coordinates acting at the
      % robot's end effector

      nq = obj.getNumPositions;
      q=x(1:nq); 
      
      if (nargout>1)
        nx = obj.getNumStates;
        nu = obj.getNumInputs;

        kinsol = doKinematics(obj, q, [], struct('compute_gradients', true));
        [~,J,dJ] = obj.forwardKin(kinsol,findLinkId(obj,obj.hand_name),[0;0;0]);
        uw = J'*w;
 
        dJtw = zeros(nq,nq);
        for i=1:nq
          dJtw(:,i) = dJ(:,(i-1)*nq+(1:nq))'*w;
        end
      
        [f,df] = dynamics(obj,t,x,u+uw);
        df_du = df(:,1+nx+(1:nu)); 
        df_dq = df(:,1+(1:nq)) + df_du*dJtw;
        df_dqd = df(:,1+nq+(1:nq));
        df_dw = df_du*J';
        
        df = [df(:,1),df_dq,df_dqd,df_du,df_dw];
      else
        kinsol = doKinematics(obj, q, []);
        [~,J] = obj.forwardKin(kinsol,findLinkId(obj,obj.hand_name),[0;0;0]);
        uw = J'*w;
      
        [f,df] = dynamics(obj,t,x,u+uw);
      end

    end
    
    function [f,df] = dynamics_w_x(obj,t,x,u,w)
      % w is a state error vector
      [f,df] = dynamics(obj,t,x+w,u);
      df = [df,df(:,1+(1:obj.getNumStates))];
    end
    
    function c = getZeroConfiguration(obj)
      c=zeros(obj.getNumStates,1);
    end
    
    function [f,df] = dynamics_w_u(obj,t,x,u,w)
      % u is a state error vector
      [f,df] = dynamics(obj,t,x,u+w);
      df = [df,df(:,1+obj.getNumStates+(1:obj.getNumInputs))];
    end 
    
    function u0 = findTrim(obj,q0)
        Nq = obj.getNumPositions();
        Nq_arm = 8;
        Nu = obj.getNumInputs();
        Nv = obj.getNumVelocities();
        Nx = Nq+Nv;
        
        [H,C,B] = manipulatorDynamics(obj,q0,zeros(Nv,1));
        
        u0 = B(1:Nq_arm,:)\C(1:Nq_arm);
    end

    function [phi,normal,d,xA,xB,idxA,idxB,mu,n,D,dn,dD] = contactConstraints(obj,kinsol,allow_multiple_contacts,active_collision_options)

      % @retval phi (m x 1) Vector of gap function values (typically contact distance), for m possible contacts
      % @retval normal (3 x m) Contact normal vector in world coordinates, points from B to A
      % @retval d {k} (3 x m) Contact friction basis vectors in world coordinates, points from B to A
      % @retval xA (3 x m) The closest point on body A to contact with body B, relative to body A origin and in body A frame
      % @retval xB (3 x m) The closest point on body B to contact with body A, relative to body B origin and in body B frame
      % @retval idxA (m x 1) The index of body A. 0 is the special case for the environment/terrain
      % @retval idxB (m x 1) The index of body B. 0 is the special case for the environment/terrain
      % @retval mu (m x 1) Coefficients of friction
      % @retval n (m x n) normal vector in joint coordinates, state vector length n
      % @retval D {2k}(m x n) friction cone basis in joint coordinates, for k directions
      % @retval dn (mn x n) dn/dq derivative
      % @retval dD {2k}(mn x n) dD/dq derivative

      compute_first_derivative = nargout > 8;
      compute_kinematics_gradients = nargout > 10;

      if ~isstruct(kinsol)
        % treat input as contactPositions(obj,q)
        kin_options = struct('compute_gradients', compute_kinematics_gradients);
        kinsol = doKinematics(obj, kinsol, [], kin_options);
      end
      
      % Scott's implementation
      brick_size = 0.06;
      finger_contact_left = [0;0;.04];
      finger_contact_right = [0;  0.0400;  0.1225];%-0.0001

      box_pose = obj.forwardKin(kinsol,obj.brick_id,[0;0;0],1);
      left_finger_tip = obj.forwardKin(kinsol,obj.left_finger_id,finger_contact_left,0);
      right_finger_tip = obj.forwardKin(kinsol,obj.right_finger_id,finger_contact_right,0);

      xA = [finger_contact_right, finger_contact_left];
      idxA = [obj.right_finger_id; obj.left_finger_id];
      idxB = [obj.brick_id; obj.brick_id];
      mu = 1.0;
      
      R_box = rpy2rotmat(box_pose(4:6));
      right_normal = R_box*[1;0;0];
      left_normal = R_box*[-1;0;0];
      
      normal = [right_normal, left_normal];
      d = cell(1,2);
      d{1} = [R_box*[0;1;0],R_box*[0;1;0]];
      d{2} = [R_box*[0;0;1],R_box*[0;0;1]];

      right_finger_in_B = right_finger_tip-box_pose(1:3);
      left_finger_in_B = left_finger_tip-box_pose(1:3);
      
      phi_right = right_finger_in_B'*right_normal - brick_size/2.0;
      phi_left = left_finger_in_B'*left_normal - brick_size/2.0;
      xB = [right_finger_in_B - right_finger_in_B'*right_normal *right_normal + [brick_size/2;0;0], ...
        left_finger_in_B - left_finger_in_B'*left_normal * left_normal - [brick_size/2;0;0]];
      
      phi = [phi_right;phi_left];
      
%       % Zac's implementation
%       ball_radius = 0.03;
%       finger_contact_left = [0;0;.04];
%       finger_contact_right1 = [-.01;  0.0400;  0.1225];
%       finger_contact_right2 = [.01;  0.0400;  0.1225];
%       
%       b = obj.forwardKin(kinsol,obj.brick_id,[0;0;0],1);
%       R_ball = rpy2rotmat(b(4:6));
%       fl = obj.forwardKin(kinsol,obj.left_finger_id,finger_contact_left,1);
%       fr1 = obj.forwardKin(kinsol,obj.right_finger_id,finger_contact_right1,1);
%       fr2 = obj.forwardKin(kinsol,obj.right_finger_id,finger_contact_right2,1);
%       
%       phi = [b(3)-ball_radius; norm(fr1(1:3)-b(1:3))-ball_radius; norm(fr2(1:3)-b(1:3))-ball_radius; norm(fl(1:3)-b(1:3))-ball_radius];
%       ball_normal = [0;0;-1];
%       right_normal1 = fr1(1:3) - b(1:3);
%       right_normal1 = right_normal1./sqrt(right_normal1'*right_normal1);
%       right_normal2 = fr2(1:3) - b(1:3);
%       right_normal2 = right_normal2./sqrt(right_normal2'*right_normal2);
%       left_normal = fl(1:3) - b(1:3);
%       left_normal = left_normal./sqrt(left_normal'*left_normal);
%       normal = [ball_normal, right_normal1, right_normal2, left_normal];
%       
%       d = cell(1,2);
%       Tr1 = cross(right_normal1,[0;0;1]);
%       Tr1 = Tr1/norm(Tr1);
%       Tr2 = cross(right_normal1,Tr1);
%       Tr3 = cross(right_normal2,[0;0;1]);
%       Tr3 = Tr3/norm(Tr3);
%       Tr4 = cross(right_normal2,Tr3);
%       Tl1 = cross(left_normal,[0;0;1]);
%       Tl1 = Tl1/norm(Tl1);
%       Tl2 = cross(left_normal,Tl1);
%       d{1} = [[0;1;0],Tr1,Tr3,Tl1];
%       d{2} = [[1;0;0],Tr2,Tr4,Tl2];
%       
%       xA = [[b(1:2); 0], finger_contact_right1, finger_contact_right2, finger_contact_left];
%       xB = ball_radius*R_ball'*normal;
%       idxA = [0; obj.right_finger_id; obj.right_finger_id; obj.left_finger_id];
%       idxB = [obj.brick_id; obj.brick_id; obj.brick_id; obj.brick_id];
%       mu = 1.0;

%       if phi<0
%         keyboard
%       end
      if compute_kinematics_gradients
        [n, D, dn, dD] = contactConstraintDerivatives(obj, normal, kinsol, idxA, idxB, xA, xB, d);
      elseif compute_first_derivative
        [n, D] = contactConstraintDerivatives(obj, normal, kinsol, idxA, idxB, xA, xB, d);
      end
    end
  end
  
end

