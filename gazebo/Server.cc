/*
 * Copyright 2011 Nate Koenig
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/
#include <stdio.h>
#include <signal.h>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>

#include "gazebo/gazebo.hh"
#include "gazebo/transport/transport.hh"

#include "gazebo/common/Logger.hh"
#include "gazebo/common/LogPlay.hh"
#include "gazebo/common/Timer.hh"
#include "gazebo/common/Exception.hh"
#include "gazebo/common/Plugin.hh"
#include "gazebo/common/Common.hh"

#include "gazebo/sdf/sdf.hh"

#include "gazebo/sensors/Sensors.hh"

#include "gazebo/physics/Physics.hh"
#include "gazebo/physics/World.hh"
#include "gazebo/physics/Base.hh"

#include "gazebo/Master.hh"
#include "gazebo/Server.hh"

using namespace gazebo;

bool Server::stop = true;

/////////////////////////////////////////////////
Server::Server()
{
  this->receiveMutex = new boost::mutex();
  gazebo::print_version();

  if (signal(SIGINT, Server::SigInt) == SIG_ERR)
    std::cerr << "signal(2) failed while setting up for SIGINT" << std::endl;
}

/////////////////////////////////////////////////
Server::~Server()
{
  fflush(stdout);
  delete this->receiveMutex;
  delete this->master;
}

/////////////////////////////////////////////////
void Server::PrintUsage()
{
  std::cerr << "Run the Gazebo server.\n\n"
    << "Usage: gzserver [options] <world_file>\n\n";
}

/////////////////////////////////////////////////
bool Server::ParseArgs(int argc, char **argv)
{
  // save a copy of argc and argv for consumption by system plugins
  this->systemPluginsArgc = argc;
  this->systemPluginsArgv = new char*[argc];
  for (int i = 0; i < argc; ++i)
  {
    int argv_len = strlen(argv[i]);
    this->systemPluginsArgv[i] = new char[argv_len];
    for (int j = 0; j < argv_len; ++j)
      this->systemPluginsArgv[i][j] = argv[i][j];
  }


  po::options_description v_desc("Allowed options");
  v_desc.add_options()
    ("help,h", "Produce this help message.")
    ("log,l", "Log state data to disk.")
    ("play,p", po::value<std::string>(), "Playback a log file.")
    ("pause,u", "Start the server in a paused state.")
    ("server-plugin,s", po::value<std::vector<std::string> >(),
     "Load a plugin.");

  po::options_description h_desc("Hidden options");
  h_desc.add_options()
    ("world_file", po::value<std::string>(), "SDF world to load.");

  h_desc.add_options()
    ("pass_through", po::value<std::vector<std::string> >(),
     "not used, passed through to system plugins.");

  po::options_description desc("Allowed options");
  desc.add(v_desc).add(h_desc);

  po::positional_options_description p_desc;
  p_desc.add("world_file", 1).add("pass_through", -1);

  try
  {
    po::store(
        po::command_line_parser(argc, argv).options(desc).positional(
          p_desc).allow_unregistered().run(), this->vm);
    po::notify(this->vm);
  }
  catch(boost::exception &_e)
  {
    std::cerr << "Error. Invalid arguments\n";
    // NOTE: boost::diagnostic_information(_e) breaks lucid
    // std::cerr << boost::diagnostic_information(_e) << "\n";
    return false;
  }

  if (this->vm.count("help"))
  {
    this->PrintUsage();
    std::cerr << v_desc << "\n";
    return false;
  }

  /// Load all the plugins specified on the command line
  if (this->vm.count("server-plugin"))
  {
    std::vector<std::string> pp =
      this->vm["server-plugin"].as<std::vector<std::string> >();

    for (std::vector<std::string>::iterator iter = pp.begin();
         iter != pp.end(); ++iter)
    {
      gazebo::add_plugin(*iter);
    }
  }

  // Set the parameter to record a log file
  if (this->vm.count("log"))
    this->params["log"] = "true";

  if (this->vm.count("pause"))
    this->params["pause"] = "true";
  else
    this->params["pause"] = "false";

  // The following "if" block must be processed directly before
  // this->ProcessPrarams.
  //
  // Set the parameter to playback a log file. The log file contains the
  // world description, so don't try to reead the world file from the
  // command line.
  if (this->vm.count("play"))
  {
    // Load the log file
    common::LogPlay::Instance()->Open(this->vm["play"].as<std::string>());

    // Get the SDF world description from the log file
    std::string sdfString;
    common::LogPlay::Instance()->Step(sdfString);

    // Load the server
    if (!this->LoadString(sdfString))
      return false;
  }
  else
  {
    // Get the world file name from the command line, or use "empty.world"
    // if no world file is specified.
    std::string configFilename = "worlds/empty.world";
    if (this->vm.count("world_file"))
      configFilename = this->vm["world_file"].as<std::string>();

    // Load the server
    if (!this->LoadFile(configFilename))
      return false;
  }

  this->ProcessParams();
  this->Init();

  return true;
}

/////////////////////////////////////////////////
bool Server::GetInitialized() const
{
  return !this->stop && !transport::is_stopped();
}

/////////////////////////////////////////////////
bool Server::LoadFile(const std::string &_filename)
{
  // Quick test for a valid file
  FILE *test = fopen(common::find_file(_filename).c_str(), "r");
  if (!test)
  {
    gzerr << "Could not open file[" << _filename << "]\n";
    return false;
  }
  fclose(test);

  // Load the world file
  sdf::SDFPtr sdf(new sdf::SDF);
  if (!sdf::init(sdf))
  {
    gzerr << "Unable to initialize sdf\n";
    return false;
  }

  if (!sdf::readFile(_filename, sdf))
  {
    gzerr << "Unable to read sdf file[" << _filename << "]\n";
    return false;
  }

  return this->LoadImpl(sdf->root);
}

/////////////////////////////////////////////////
bool Server::LoadString(const std::string &_sdfString)
{
  // Load the world file
  sdf::SDFPtr sdf(new sdf::SDF);
  if (!sdf::init(sdf))
  {
    gzerr << "Unable to initialize sdf\n";
    return false;
  }

  if (!sdf::readString(_sdfString, sdf))
  {
    gzerr << "Unable to read SDF string[" << _sdfString << "]\n";
    return false;
  }

  return this->LoadImpl(sdf->root);
}

/////////////////////////////////////////////////
bool Server::LoadImpl(sdf::ElementPtr _elem)
{
  std::string host = "";
  unsigned int port = 0;

  gazebo::transport::get_master_uri(host, port);

  this->master = new gazebo::Master();
  this->master->Init(port);
  this->master->RunThread();


  // Load gazebo
  gazebo::load(this->systemPluginsArgc, this->systemPluginsArgv);

  /// Load the sensors library
  sensors::load();

  /// Load the physics library
  physics::load();

  sdf::ElementPtr worldElem = _elem->GetElement("world");
  if (worldElem)
  {
    physics::WorldPtr world = physics::create_world();

    // Create the world
    try
    {
      physics::load_world(world, worldElem);
    }
    catch(common::Exception &e)
    {
      gzthrow("Failed to load the World\n"  << e);
    }
  }

  this->node = transport::NodePtr(new transport::Node());
  this->node->Init("/gazebo");
  this->serverSub = this->node->Subscribe("/gazebo/server/control",
                                          &Server::OnControl, this);

  this->worldModPub =
    this->node->Advertise<msgs::WorldModify>("/gazebo/world/modify");

  // Run the gazebo, starts a new thread
  gazebo::run();

  return true;
}

/////////////////////////////////////////////////
void Server::Init()
{
  gazebo::init();

  sensors::init();

  physics::init_worlds();
  this->stop = false;
}

/////////////////////////////////////////////////
void Server::SigInt(int)
{
  stop = true;
}

/////////////////////////////////////////////////
void Server::Stop()
{
  this->stop = true;
}

/////////////////////////////////////////////////
void Server::Fini()
{
  this->Stop();

  gazebo::fini();

  physics::fini();

  sensors::fini();

  if (this->master)
    this->master->Fini();
  delete this->master;
  this->master = NULL;
}

/////////////////////////////////////////////////
void Server::Run()
{
  if (this->stop)
    return;

  // Make sure the sensors are updated once before running the world.
  // This makes sure plugins get loaded properly.
  sensors::run_once(true);

  // Run each world. Each world starts a new thread
  physics::run_worlds();

  // Update the sensors.
  while (!this->stop)
  {
    this->ProcessControlMsgs();
    sensors::run_once(true);
    common::Time::MSleep(1);
  }

  // Stop all the worlds
  physics::stop_worlds();

  sensors::stop();

  // Stop gazebo
  gazebo::stop();

  // Stop the master
  this->master->Stop();
}

/////////////////////////////////////////////////
void Server::ProcessParams()
{
  common::StrStr_M::const_iterator iter;
  for (iter = this->params.begin(); iter != this->params.end(); ++iter)
  {
    if (iter->first == "pause")
    {
      bool p = false;
      try
      {
        p = boost::lexical_cast<bool>(iter->second);
      }
      catch(...)
      {
        // Unable to convert via lexical_cast, so try "true/false" string
        std::string str = iter->second;
        boost::to_lower(str);

        if (str == "true")
          p = true;
        else if (str == "false")
          p = false;
        else
          gzerr << "Invalid param value[" << iter->first << ":"
                << iter->second << "]\n";
      }

      physics::pause_worlds(p);
    }
    else if (iter->first == "log")
    {
      common::Logger::Instance()->Start();
    }
  }
}

/////////////////////////////////////////////////
void Server::SetParams(const common::StrStr_M &_params)
{
  common::StrStr_M::const_iterator iter;
  for (iter = _params.begin(); iter != _params.end(); ++iter)
    this->params[iter->first] = iter->second;
}

/////////////////////////////////////////////////
void Server::OnControl(ConstServerControlPtr &_msg)
{
  boost::mutex::scoped_lock lock(*this->receiveMutex);
  this->controlMsgs.push_back(*_msg);
}

/////////////////////////////////////////////////
void Server::ProcessControlMsgs()
{
  std::list<msgs::ServerControl>::iterator iter;
  for (iter = this->controlMsgs.begin();
       iter != this->controlMsgs.end(); ++iter)
  {
    if ((*iter).has_save_world_name())
    {
      physics::WorldPtr world = physics::get_world((*iter).save_world_name());
      if ((*iter).has_save_filename())
        world->Save((*iter).save_filename());
      else
        gzerr << "No filename specified.\n";
    }
    else if ((*iter).has_new_world() && (*iter).new_world())
    {
      this->OpenWorld("worlds/empty.world");
    }
    else if ((*iter).has_open_filename())
    {
      this->OpenWorld((*iter).open_filename());
    }
  }
  this->controlMsgs.clear();
}

/////////////////////////////////////////////////
bool Server::OpenWorld(const std::string &_filename)
{
  sdf::SDFPtr sdf(new sdf::SDF);
  if (!sdf::init(sdf))
  {
    gzerr << "Unable to initialize sdf\n";
    return false;
  }

  if (!sdf::readFile(_filename, sdf))
  {
    gzerr << "Unable to read sdf file[" << _filename << "]\n";
    return false;
  }

  msgs::WorldModify worldMsg;
  worldMsg.set_world_name("default");
  worldMsg.set_remove(true);
  this->worldModPub->Publish(worldMsg);

  physics::stop_worlds();

  physics::remove_worlds();

  sensors::remove_sensors();

  gazebo::transport::clear_buffers();

  sdf::ElementPtr worldElem = sdf->root->GetElement("world");

  physics::WorldPtr world = physics::create_world();

  physics::load_world(world, worldElem);

  physics::init_world(world);

  physics::run_world(world);

  worldMsg.set_world_name("default");
  worldMsg.set_remove(false);
  worldMsg.set_create(true);
  this->worldModPub->Publish(worldMsg);
  return true;
}
