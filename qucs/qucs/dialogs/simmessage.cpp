/***************************************************************************
                              simmessage.cpp
                             ----------------
    begin                : Sat Sep 6 2003
    copyright            : (C) 2003 by Michael Margraf
    email                : michael.margraf@alumni.tu-berlin.de
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "simmessage.h"
#include "main.h"
#include "qucs.h"
#include "qucsdoc.h"

#include <qlabel.h>
#include <qlayout.h>
#include <qvgroupbox.h>
#include <qhgroupbox.h>
#include <qhbox.h>
#include <qtimer.h>
#include <qpushbutton.h>
#include <qprogressbar.h>
#include <qtextedit.h>
#include <qdatetime.h>
#include <qregexp.h>


SimMessage::SimMessage(QucsDoc *Doc_, QWidget *parent)
		: QDialog(parent, 0, FALSE, Qt::WDestructiveClose)
{
  Doc = Doc_;
  QFileInfo Info(Doc->DocName);
  DataDisplay = Info.dirPath() + QDir::separator();
  DataSet = DataDisplay + Doc->DataSet;
  DataDisplay += Doc->DataDisplay;
  showBias = Doc->showBias;     // save some settings as the document...
  SimOpenDpl = Doc->SimOpenDpl; // ...could be closed during the simulation.
  setCaption(tr("Qucs Simulation Messages"));

  all = new QVBoxLayout(this);
  all->setSpacing(5);
  all->setMargin(5);
  QVGroupBox *Group1 = new QVGroupBox(tr("Progress:"),this);
  all->addWidget(Group1);

  ProgText = new QTextEdit(Group1);
  ProgText->setTextFormat(Qt::PlainText);
  ProgText->setReadOnly(true);
  ProgText->setWordWrap(QTextEdit::NoWrap);
  ProgText->setMinimumSize(400,80);
  wasLF = false;

  QHGroupBox *HGroup = new QHGroupBox(this);
  HGroup->setInsideMargin(5);
  HGroup->setInsideSpacing(5);
  all->addWidget(HGroup);
  new QLabel(tr("Progress:"), HGroup);
  SimProgress = new QProgressBar(HGroup);
//  SimProgress->setPercentageVisible(false);

  QVGroupBox *Group2 = new QVGroupBox(tr("Errors and Warnings:"),this);
  all->addWidget(Group2);

  ErrText = new QTextEdit(Group2);
  ErrText->setTextFormat(Qt::PlainText);
  ErrText->setReadOnly(true);
  ErrText->setWordWrap(QTextEdit::NoWrap);
  ErrText->setMinimumSize(400,80);

  QHBox *Butts = new QHBox(this);
  all->addWidget(Butts);

  Display = new QPushButton(tr("Goto display page"), Butts);
  Display->setDisabled(true);
  connect(Display,SIGNAL(clicked()),SLOT(slotDisplayButton()));

  Abort = new QPushButton(tr("Abort simulation"), Butts);
  connect(Abort,SIGNAL(clicked()),SLOT(reject()));
}

SimMessage::~SimMessage()
{
  if(SimProcess.isRunning())  SimProcess.kill();
  delete all;
}

// ------------------------------------------------------------------------
bool SimMessage::startProcess()
{
  Abort->setText(tr("Abort simulation"));
  Display->setDisabled(true);

  ProgText->insert(tr("Starting new simulation on ")+
                   QDate::currentDate().toString("ddd dd. MMM yyyy"));
  ProgText->insert(tr(" at ")+
                   QTime::currentTime().toString("hh:mm:ss")+"\n\n");

  SimProcess.blockSignals(false);
  if(SimProcess.isRunning()) {
    ErrText->insert(tr("ERROR: Simulator is still running!"));
    FinishSimulation(-1);
    return false;
  }

  ProgText->insert(tr("creating netlist... "));
  NetlistFile.setName(QucsHomeDir.filePath("netlist.txt"));
  if(!NetlistFile.open(IO_WriteOnly)) {
    ErrText->insert(tr("ERROR: Cannot write netlist file!"));
    FinishSimulation(-1);
    return false;
  }

  Collect.clear();  // clear list for NodeSets, SPICE components etc.
  Stream.setDevice(&NetlistFile);
  SimPorts = Doc->File.prepareNetlist(Stream, Collect, ErrText);
  if(SimPorts < -5) {
    NetlistFile.close();
    FinishSimulation(-1);
    return false;
  }
  Collect.append("*");   // mark the end


  disconnect(&SimProcess, 0, 0, 0);
  connect(&SimProcess, SIGNAL(readyReadStderr()), SLOT(slotDisplayErr()));
  connect(&SimProcess, SIGNAL(readyReadStdout()),
                       SLOT(slotReadSpiceNetlist()));
  connect(&SimProcess, SIGNAL(processExited()),
                       SLOT(slotFinishSpiceNetlist()));

  nextSPICE();
  return true;
  // Since now, the Doc pointer may be obsolete, as the user could have
  // closed the schematic !!!
}
  
// ---------------------------------------------------
// Converts a spice netlist into Qucs format and outputs it.
void SimMessage::nextSPICE()
{
  QString Line;
  for(;;) {  // search for next SPICE component
    Line = *(Collect.begin());
    Collect.remove(Collect.begin());
    if(Line == "*") {  // worked on all components ?
      startSimulator(); // <<<<<================== go on ===
      return;
    }
    if(Line.left(5) == "SPICE") {
      if(Line.at(5) != 'o') insertSim = true;
      else insertSim = false;
      break;
    }
    Collect.append(Line);
  }


  QString FileName = Line.section('"', 1,1);
  Line = Line.section('"', 2);  // port nodes
  if(Line.isEmpty())  makeSubcircuit = false;
  else  makeSubcircuit = true;

  QStringList com;
  com << (QucsSettings.BinDir + "qucsconv");
  if(makeSubcircuit)
    com << "-g" << "_ref";
  com << "-if" << "spice" << "-of" << "qucs" << "-i";
  if(FileName.find(QDir::separator()) < 0)  // add path ?
    com << QucsWorkDir.path() + QDir::separator() + FileName;
  else
    com << FileName;
  SimProcess.setArguments(com);


  if(makeSubcircuit) {
    Stream << "\n.Def:" << properName(FileName) << " ";
  
    Line.replace(',', ' ');
    Stream << Line;
    if(!Line.isEmpty()) Stream << " _ref";
  }
  Stream << "\n";


  ProgressText = "";
  if(!SimProcess.start()) {
    ErrText->insert(tr("ERROR: Cannot start QucsConv!"));
    FinishSimulation(-1);
    return;
  }
  SimProcess.closeStdin();
}

// ------------------------------------------------------------------------
void SimMessage::slotReadSpiceNetlist()
{
  int i;
  QString s;
  ProgressText += QString(SimProcess.readStdout());

  while((i = ProgressText.find('\n')) >= 0) {

    s = ProgressText.left(i);
    ProgressText.remove(0, i+1);


    s = s.stripWhiteSpace();
    if(s.isEmpty()) continue;
    if(s.at(0) == '#') continue;
    if(s.at(0) == '.') if(s.left(5) != ".Def:") { // insert simulations later
      if(insertSim) Collect.append(s);
      continue;
    }
    Stream << "  " << s << '\n';
  }
}


// ------------------------------------------------------------------------
void SimMessage::slotFinishSpiceNetlist()
{
  if(makeSubcircuit)
    Stream << ".Def:End\n\n";

  nextSPICE();
}

// ------------------------------------------------------------------------
void SimMessage::startSimulator()
{
  // Using the Doc pointer here is risky as the user may have closed
  // the schematic, but converting the SPICE netlists is (hopefully)
  // faster than the user (I have no other idea).

  // output NodeSets, SPICE simulations etc.
  Stream << Collect.join("\n") << '\n';
  QString SimTime =
    Doc->File.createNetlist(Stream, SimPorts);
  NetlistFile.close();
  ProgText->insert(tr("done.\n"));

  QStringList com;
  if(SimPorts < 0)
    com << QucsSettings.BinDir + "qucsator" << "-b" << "-g" << "-i"
        << QucsHomeDir.filePath("netlist.txt") << "-o" << DataSet;
  else
    com << QucsSettings.BinDir + "qucsdigi" << "netlist.txt" << SimTime
        << QucsHomeDir.absPath();

  SimProcess.setArguments(com);


  disconnect(&SimProcess, 0, 0, 0);
  connect(&SimProcess, SIGNAL(readyReadStderr()), SLOT(slotDisplayErr()));
  connect(&SimProcess, SIGNAL(readyReadStdout()), SLOT(slotDisplayMsg()));
  connect(&SimProcess, SIGNAL(processExited()), SLOT(slotSimEnded()));

#ifdef SPEEDUP_PROGRESSBAR
  waitForUpdate = false;
#endif
  wasLF = false;
  ProgressText = "";
  if(!SimProcess.start()) {
    ErrText->insert(tr("ERROR: Cannot start simulator!"));
    FinishSimulation(-1);
    return;
  }
}

// ------------------------------------------------------------------------
// Is called when the process sends an output to stdout.
void SimMessage::slotDisplayMsg()
{
  int i;
  ProgressText += QString(SimProcess.readStdout());
  if(wasLF) {
    i = ProgressText.findRev('\r');
    if(i > 1) {
#ifdef SPEEDUP_PROGRESSBAR
      iProgress = 10*int(ProgressText.at(i-2).latin1()-'0') +
                     int(ProgressText.at(i-1).latin1()-'0');
      if(!waitForUpdate) {
        QTimer::singleShot(20, this, SLOT(slotUpdateProgressBar()));
        waitForUpdate = true;
      }
#else
      SimProgress->setProgress(
         10*int(ProgressText.at(i-2).latin1()-'0') +
            int(ProgressText.at(i-1).latin1()-'0'), 100);
#endif
      ProgressText.remove(0, i+1);
    }

    if(ProgressText.at(0).latin1() <= '\t')
      return;
  }
  else {
    i = ProgressText.find('\t');
    if(i >= 0) {
      wasLF = true;
      ProgText->insert(ProgressText.left(i) + "\n");
      ProgressText.remove(0, i+1);
      return;
    }
  }

  ProgText->insert(ProgressText);
  ProgressText = "";
  wasLF = false;

#if 0
  char *p;
  int i, Progress;
  QByteArray Data = SimProcess.readStdout();
  if(wasLF) {
    ProgressText += QString(Data);
    i = ProgressText.findRev('\r');
    if(i > 1) {
      Progress = 10*int(ProgressText.at(i-2).latin1()-'0') +
                    int(ProgressText.at(i-1).latin1()-'0');
      if(Progress < 0)  Progress += 160;
      SimProgress->setProgress(Progress, 100);
      ProgressText.remove(0, i+1);
    }

    if(ProgressText.at(0).latin1() > '\t') {
      ProgText->insert(ProgressText);
      wasLF = false;
//qDebug("Ende: "+ProgressText);
    }
  }
  else {
    i = Data.find('\t');
    if(i < 0)
      ProgText->insert(QString(Data));
    else {
      wasLF = true;
      p = Data.data();
      *(p+i) = 0;
      ProgText->insert(QString(p) + "\n");
      ProgressText = QString(Data).mid(i+1);
//qDebug("Beginn: "+ProgressText);
    }
  }
#endif
}

#ifdef SPEEDUP_PROGRESSBAR
// ------------------------------------------------------------------------
void SimMessage::slotUpdateProgressBar()
{
  SimProgress->setProgress(iProgress, 100);
  waitForUpdate = false;
}
#endif

// ------------------------------------------------------------------------
// Is called when the process sends an output to stderr.
void SimMessage::slotDisplayErr()
{
  int par = ErrText->paragraphs();
  int idx = ErrText->paragraphLength(par-1);
  ErrText->setCursorPosition(par-1,idx);
  ErrText->insert(QString(SimProcess.readStderr()));
}

// ------------------------------------------------------------------------
// Is called when the simulation process terminates.
void SimMessage::slotSimEnded()
{
  int stat = (!SimProcess.normalExit()) ? -1 : SimProcess.exitStatus();
  FinishSimulation(stat);
}

// ------------------------------------------------------------------------
// Is called when the simulation ended with errors before starting simulator
// process.
void SimMessage::FinishSimulation(int Status)
{
  Abort->setText(tr("Close window"));
  Display->setDisabled(false);
  SimProgress->setProgress(100, 100);   // progress bar to 100%

  QDate d = QDate::currentDate();   // get date of today
  QTime t = QTime::currentTime();   // get time

  if(Status == 0) {
    ProgText->insert(tr("\nSimulation ended on ")+
                     d.toString("ddd dd. MMM yyyy"));
    ProgText->insert(tr(" at ")+t.toString("hh:mm:ss")+"\n");
    ProgText->insert(tr("Ready.\n"));
  }
  else {
    ProgText->insert(tr("\nErrors occured during simulation on ")+
                     d.toString("ddd dd. MMM yyyy"));
    ProgText->insert(tr(" at ")+t.toString("hh:mm:ss")+"\n");
    ProgText->insert(tr("Aborted.\n"));
  }

  QFile file(QucsHomeDir.filePath("log.txt"));  // save simulator messages
  if(file.open(IO_WriteOnly)) {
    int z;
    QTextStream stream(&file);
    stream << tr("Output:\n----------\n\n");
    for(z=0; z<=ProgText->paragraphs(); z++)
      stream << ProgText->text(z) << "\n";
    stream << tr("\n\n\nErrors:\n--------\n\n");
    for(z=0; z<ErrText->paragraphs(); z++)
      stream << ErrText->text(z) << "\n";
    file.close();
  }

  emit SimulationEnded(Status, this);
}

// ------------------------------------------------------------------------
// To call accept(), which is protected, from the outside.
void SimMessage::slotClose()
{
  accept();
}

// ------------------------------------------------------------------------
void SimMessage::slotDisplayButton()
{
  emit displayDataPage(DataDisplay);
  accept();
}
