'''
Usage: This program should be launched using the command line specified in the
       kuka_sim.pmd file.
'''

from director import mainwindowapp
from director import robotsystem
from director import applogic
from director import lcmUtils
from PythonQt import QtGui, QtCore
import drake as lcm_drake
import subprocess

import os
import lcm
import sys
sys.path.append('../')
from director_ext import genericdrakeik
import Robotiq_Command

class KukaSimInfoLabel(object):
    '''
    Displays simulation time and frequency in the status bar
    using information from the IIWA_STATUS lcm channel.
    '''

    def __init__(self, statusBar):
        self.label = QtGui.QLabel('')
        statusBar.addPermanentWidget(self.label)

        self.sub = lcmUtils.addSubscriber('IIWA_STATUS',
                                  lcm_drake.lcmt_iiwa_status, self.onIiwaStatus)
        self.sub.setSpeedLimit(30)

        self.label.text = '[waiting for sim status]'

    def onIiwaStatus(self, msg):
        simTime = msg.utime*1e-6
        simFreq = self.sub.getMessageRate()
        self.label.text = 'Sim freq: %d hz  |  Sim time: %.2f' % (simFreq,
                                                                  simTime)


def makeRobotSystem(view):
    factory = robotsystem.RobotSystemFactory()
    options = factory.getDisabledOptions()
    factory.setDependentOptions(options, usePlannerPublisher=True,
                                         useTeleop=True)
    return factory.construct(view=view, options=options)

def sendGripperCommand(action):

  lc = lcm.LCM()
  command_topic = outputMsg()
  command = outputMsg()
  command = CModelSimpleController.genCommand(action,command) 
  lc.publish("command_topic",command.encode())

def gripperConnect():
  pass
def gripperOpen():
  tester.gripperOpen()
def gripperClose():
  tester.gripperClose()
def gripperReset():
  tester.gripperReset()
def gripperActivate():
  tester.gripperActivate()
  
def setupGripperButtons():
  toolBar = applogic.findToolBar('Main Toolbar')
  app.app.addToolBarAction(toolBar, 'Gripper Open', icon='', callback=gripperOpen)
  app.app.addToolBarAction(toolBar, 'Gripper Close', icon='', callback=gripperClose)
  app.app.addToolBarAction(toolBar, 'Gripper Reset', icon='', callback=gripperReset)
  app.app.addToolBarAction(toolBar, 'Gripper Connect', icon='', callback=gripperConnect)
  app.app.addToolBarAction(toolBar, 'Gripper Activate', icon='', callback=gripperActivate)

# create a default mainwindow app
app = mainwindowapp.MainWindowAppFactory().construct()
mainwindowapp.MainWindowPanelFactory().construct(app=app.app, view=app.view)

# load a minimal robot system with ik planning
robotSystem = makeRobotSystem(app.view)

# add the teleop and playback panels to the mainwindow
app.app.addWidgetToDock(robotSystem.teleopPanel.widget,
                        QtCore.Qt.RightDockWidgetArea)
app.app.addWidgetToDock(robotSystem.playbackPanel.widget,
                        QtCore.Qt.BottomDockWidgetArea)

setupGripperButtons()

# show sim time in the status bar
infoLabel = KukaSimInfoLabel(app.mainWindow.statusBar())

# use generic ik backend
ikPlanner = robotSystem.ikPlanner
genericPlannerPub = genericdrakeik.GenericDrakePlannerPublisher(ikPlanner, robotSystem.affordanceManager)
ikPlanner.addPublisher('generic', genericPlannerPub)
ikPlanner.planningMode = 'generic'

# change the default animation mode of the playback panel
robotSystem.playbackPanel.animateOnExecute = True

# disable pointwise ik by default
robotSystem.ikPlanner.getIkOptions().setProperty('Use pointwise', False)

# set the default camera view
applogic.resetCamera(viewDirection=[-1,0,0], view=app.view)

tester = Robotiq_Command.Robotiq_Command()

# start!
app.app.start()
