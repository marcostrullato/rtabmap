/*
Copyright (c) 2010-2016, Mathieu Labbe - IntRoLab - Universite de Sherbrooke
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the Universite de Sherbrooke nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <rtabmap/core/Rtabmap.h>
#include <rtabmap/core/DBDriver.h>
#include <rtabmap/core/DBReader.h>
#ifdef RTABMAP_OCTOMAP
#include <rtabmap/core/OctoMap.h>
#endif
#include <rtabmap/core/OccupancyGrid.h>
#include <rtabmap/core/Graph.h>
#include <rtabmap/core/Memory.h>
#include <rtabmap/core/CameraThread.h>
#include <rtabmap/utilite/UFile.h>
#include <rtabmap/utilite/UDirectory.h>
#include <rtabmap/utilite/UTimer.h>
#include <rtabmap/utilite/UStl.h>
#include <rtabmap/utilite/UMath.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pcl/io/pcd_io.h>
#include <signal.h>

using namespace rtabmap;

void showUsage()
{
	printf("\nUsage:\n"
			"   rtabmap-reprocess [options] \"input.db\" \"output.db\"\n"
			"   rtabmap-reprocess [options] \"input1.db;input2.db;input3.db\" \"output.db\"\n"
			"\n"
			"   For the second example, only parameters from the first database are used.\n"
			"   If Mem/IncrementalMemory is false, RTAB-Map is initialized with the first input database.\n"
			"   To see warnings when loop closures are rejected, add \"--uwarn\" argument.\n"
			"   To upgrade version of an old database to newest version:\n"
			"      rtabmap-reprocess --Db/TargetVersion \"\" \"input.db\" \"output.db\"\n"
			"\n"
			"  Options:\n"
			"     -r                Use database stamps as input rate.\n"
			"     -skip #           Skip # frames after each processed frame (default 0=don't skip any frames).\n"
			"     -c \"path.ini\"   Configuration file, overwriting parameters read \n"
			"                       from the database. If custom parameters are also set as \n"
			"                       arguments, they overwrite those in config file and the database.\n"
			"     -start #    Start from this node ID.\n"
			"     -stop #     Last node to process.\n"
			"     -g2         Assemble 2D occupancy grid map and save it to \"[output]_map.pgm\". Use with -db to save in database.\n"
			"     -g3         Assemble 3D cloud map and save it to \"[output]_map.pcd\".\n"
			"     -o2         Assemble OctoMap 2D projection and save it to \"[output]_octomap.pgm\". Use with -db to save in database.\n"
			"     -o3         Assemble OctoMap 3D cloud and save it to \"[output]_octomap.pcd\".\n"
			"     -db         Save assembled 2D occupancy grid in database instead of a file.\n"
			"     -p          Save odometry and localization poses (*.g2o).\n"
			"     -scan_from_depth    Generate scans from depth images (overwrite previous\n"
			"                         scans if they exist).\n"
			"     -scan_downsample #  Downsample input scans.\n"
			"     -scan_range_min #.#   Filter input scans with minimum range (m).\n"
			"     -scan_range_max #.#   Filter input scans with maximum range (m).\n"
			"     -scan_voxel_size #.#  Voxel filter input scans (m).\n"
			"     -scan_normal_k #         Compute input scan normals (k-neighbors approach).\n"
			"     -scan_normal_radius #.#  Compute input scan normals (radius(m)-neighbors approach).\n\n"
			"%s\n"
			"\n", Parameters::showUsage());
	exit(1);
}

// catch ctrl-c
bool g_loopForever = true;
void sighandler(int sig)
{
	printf("\nSignal %d caught...\n", sig);
	g_loopForever = false;
}

int loopCount = 0;
int proxCount = 0;
int loopCountMotion = 0;
int totalFrames = 0;
int totalFramesMotion = 0;
std::vector<float> previousLocalizationDistances;
std::vector<float> odomDistances;
std::vector<float> localizationVariations;
std::vector<float> localizationAngleVariations;
std::vector<float> localizationTime;
std::map<int, Transform> odomTrajectoryPoses;
std::multimap<int, Link> odomTrajectoryLinks;
std::map<int, Transform> localizationPoses;
bool exportPoses = false;
int sessionCount = 0;
void showLocalizationStats(const std::string & outputDatabasePath)
{
	printf("Total localizations on previous session = %d/%d (Loop=%d, Prox=%d, In Motion=%d/%d)\n", loopCount+proxCount, totalFrames, loopCount, proxCount, loopCountMotion, totalFramesMotion);
	{
		float m = uMean(localizationTime);
		float var = uVariance(localizationTime, m);
		float stddev = -1;
		if(var>0)
		{
			stddev = sqrt(var);
		}
		printf("Average localization time = %f ms (stddev=%f ms)\n", m, stddev);
	}
	if(localizationVariations.size()>=2)
	{
		//ignore first localization
		localizationVariations = std::vector<float>(++localizationVariations.begin(), localizationVariations.end());
		localizationAngleVariations = std::vector<float>(++localizationAngleVariations.begin(), localizationAngleVariations.end());

		float m = uMean(localizationVariations);
		float max = uMax(localizationVariations);
		float var = uVariance(localizationVariations, m);
		float stddev = -1;
		if(var>0)
		{
			stddev = sqrt(var);
		}
		float mA = uMean(localizationAngleVariations);
		float maxA = uMax(localizationAngleVariations);
		float varA = uVariance(localizationAngleVariations, mA);
		float stddevA = -1;
		if(varA>0)
		{
			stddevA = sqrt(varA);
		}
		printf("Average localization variations = %f m, %f deg (stddev=%f m, %f deg) (max=%f m, %f deg)\n", m, mA, stddev, stddevA, max, maxA);
	}
	if(!previousLocalizationDistances.empty())
	{
		float m = uMean(previousLocalizationDistances);
		float var = uVariance(previousLocalizationDistances, m);
		float stddev = -1;
		if(var>0)
		{
			stddev = sqrt(var);
		}
		printf("Average distance from previous localization = %f m (stddev=%f m)\n", m, stddev);
	}
	if(!odomDistances.empty())
	{
		float m = uMean(odomDistances);
		float var = uVariance(odomDistances, m);
		float stddev = -1;
		if(var>0)
		{
			stddev = sqrt(var);
		}
		printf("Average odometry distances = %f m (stddev=%f m)\n", m, stddev);
	}

	if(exportPoses)
	{
		std::string outputPath = outputDatabasePath.substr(0, outputDatabasePath.size()-3);
		std::string oName = outputPath+uFormat("_session_%d_odom.g2o", sessionCount);
		std::string lName = outputPath+uFormat("_session_%d_loc.g2o", sessionCount);
		graph::exportPoses(oName, 4, odomTrajectoryPoses, odomTrajectoryLinks);
		graph::exportPoses(lName, 4, localizationPoses, odomTrajectoryLinks);
		printf("Exported %s and %s\n", oName.c_str(), lName.c_str());
	}

	loopCount = 0;
	proxCount = 0;
	totalFrames = 0;
	loopCountMotion = 0;
	totalFramesMotion = 0;
	previousLocalizationDistances.clear();
	odomDistances.clear();
	localizationVariations.clear();
	localizationAngleVariations.clear();
	localizationTime.clear();
	odomTrajectoryPoses.clear();
	odomTrajectoryLinks.clear();
	localizationPoses.clear();
	++sessionCount;
}

int main(int argc, char * argv[])
{
	signal(SIGABRT, &sighandler);
	signal(SIGTERM, &sighandler);
	signal(SIGINT, &sighandler);

	ULogger::setType(ULogger::kTypeConsole);
	ULogger::setLevel(ULogger::kError);

	ParametersMap customParameters = Parameters::parseArguments(argc, argv);

	if(argc < 3)
	{
		showUsage();
	}

	bool save2DMap = false;
	bool assemble2dMap = false;
	bool assemble3dMap = false;
	bool assemble2dOctoMap = false;
	bool assemble3dOctoMap = false;
	bool useDatabaseRate = false;
	int startId = 0;
	int stopId = 0;
	int framesToSkip = 0;
	bool scanFromDepth = false;
	int scanDecimation = 1;
	float scanRangeMin = 0.0f;
	float scanRangeMax = 0.0f;
	float scanVoxelSize = 0;
	int scanNormalK = 0;
	float scanNormalRadius = 0.0f;
	ParametersMap configParameters;
	for(int i=1; i<argc-2; ++i)
	{
		if(strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--r") == 0)
		{
			useDatabaseRate = true;
			printf("Using database stamps as input rate.\n");
		}
		else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--c") == 0)
		{
			++i;
			if (i < argc - 2 && UFile::exists(argv[i]) && UFile::getExtension(argv[i]).compare("ini") == 0)
			{
				Parameters::readINI(argv[i], configParameters);
				printf("Using %d parameters from config file \"%s\"\n", (int)configParameters.size(), argv[i]);
			}
			else if(i < argc - 2)
			{
				printf("Config file \"%s\" is not valid or doesn't exist!\n", argv[i]);
				showUsage();
			}
			else
			{
				printf("Config file is not set!\n");
				showUsage();
			}
		}
		else if (strcmp(argv[i], "-start") == 0 || strcmp(argv[i], "--start") == 0)
		{
			++i;
			if(i < argc - 2)
			{
				startId = atoi(argv[i]);
				printf("Start at node ID = %d.\n", startId);
			}
			else
			{
				printf("-start option requires a value\n");
				showUsage();
			}
		}
		else if (strcmp(argv[i], "-stop") == 0 || strcmp(argv[i], "--stop") == 0)
		{
			++i;
			if(i < argc - 2)
			{
				stopId = atoi(argv[i]);
				printf("Stop at node ID = %d.\n", stopId);
			}
			else
			{
				printf("-stop option requires a value\n");
				showUsage();
			}
		}
		else if (strcmp(argv[i], "-skip") == 0 || strcmp(argv[i], "--skip") == 0)
		{
			++i;
			if(i < argc - 2)
			{
				framesToSkip = atoi(argv[i]);
				printf("Will skip %d frames.\n", framesToSkip);
			}
			else
			{
				printf("-skip option requires a value\n");
				showUsage();
			}
		}
		else if(strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--p") == 0)
		{
			exportPoses = true;
			printf("Odometry trajectory and localization poses will be exported in g2o format (-p option).\n");
		}
		else if(strcmp(argv[i], "-db") == 0 || strcmp(argv[i], "--db") == 0)
		{
			save2DMap = true;
			printf("2D occupancy grid will be saved in database (-db option).\n");
		}
		else if(strcmp(argv[i], "-g2") == 0 || strcmp(argv[i], "--g2") == 0)
		{
			assemble2dMap = true;
			printf("2D occupancy grid will be assembled (-g2 option).\n");
		}
		else if(strcmp(argv[i], "-g3") == 0 || strcmp(argv[i], "--g3") == 0)
		{
			assemble3dMap = true;
			printf("3D cloud map will be assembled (-g3 option).\n");
		}
		else if(strcmp(argv[i], "-o2") == 0 || strcmp(argv[i], "--o2") == 0)
		{
#ifdef RTABMAP_OCTOMAP
			assemble2dOctoMap = true;
			printf("OctoMap will be assembled (-o2 option).\n");
#else
			printf("RTAB-Map is not built with OctoMap support, cannot set -o2 option!\n");
#endif
		}
		else if(strcmp(argv[i], "-o3") == 0 || strcmp(argv[i], "--o3") == 0)
		{
#ifdef RTABMAP_OCTOMAP
			assemble3dOctoMap = true;
			printf("OctoMap will be assembled (-o3 option).\n");
#else
			printf("RTAB-Map is not built with OctoMap support, cannot set -o3 option!\n");
#endif
		}
		else if (strcmp(argv[i], "-scan_from_depth") == 0 || strcmp(argv[i], "--scan_from_depth") == 0)
		{
			scanFromDepth = true;
		}
		else if (strcmp(argv[i], "-scan_downsample") == 0 || strcmp(argv[i], "--scan_downsample") == 0 ||
				strcmp(argv[i], "-scan_decimation") == 0 || strcmp(argv[i], "--scan_decimation") == 0)
		{
			++i;
			if(i < argc - 2)
			{
				scanDecimation = atoi(argv[i]);
				printf("Scan from depth decimation = %d.\n", scanDecimation);
			}
			else
			{
				printf("-scan_downsample option requires 1 value\n");
				showUsage();
			}
		}
		else if (strcmp(argv[i], "-scan_range_min") == 0 || strcmp(argv[i], "--scan_range_min") == 0)
		{
			++i;
			if(i < argc - 2)
			{
				scanRangeMin = atof(argv[i]);
				printf("Scan range min = %f m.\n", scanRangeMin);
			}
			else
			{
				printf("-scan_range_min option requires 1 value\n");
				showUsage();
			}
		}
		else if (strcmp(argv[i], "-scan_range_max") == 0 || strcmp(argv[i], "--scan_range_max") == 0)
		{
			++i;
			if(i < argc - 2)
			{
				scanRangeMax = atof(argv[i]);
				printf("Scan range max = %f m.\n", scanRangeMax);
			}
			else
			{
				printf("-scan_range_max option requires 1 value\n");
				showUsage();
			}
		}
		else if (strcmp(argv[i], "-scan_voxel_size") == 0 || strcmp(argv[i], "--scan_voxel_size") == 0)
		{
			++i;
			if(i < argc - 2)
			{
				scanVoxelSize = atof(argv[i]);
				printf("Scan voxel size = %f m.\n", scanVoxelSize);
			}
			else
			{
				printf("-scan_voxel_size option requires 1 value\n");
				showUsage();
			}
		}
		else if (strcmp(argv[i], "-scan_normal_k") == 0 || strcmp(argv[i], "--scan_normal_k") == 0)
		{
			++i;
			if(i < argc - 2)
			{
				scanNormalK = atoi(argv[i]);
				printf("Scan normal k = %d.\n", scanNormalK);
			}
			else
			{
				printf("-scan_normal_k option requires 1 value\n");
				showUsage();
			}
		}
		else if (strcmp(argv[i], "-scan_normal_radius") == 0 || strcmp(argv[i], "--scan_normal_radius") == 0)
		{
			++i;
			if(i < argc - 2)
			{
				scanNormalRadius = atof(argv[i]);
				printf("Scan normal radius = %f m.\n", scanNormalRadius);
			}
			else
			{
				printf("-scan_normal_radius option requires 1 value\n");
				showUsage();
			}
		}
	}

	std::string inputDatabasePath = uReplaceChar(argv[argc-2], '~', UDirectory::homeDir());
	std::string outputDatabasePath = uReplaceChar(argv[argc-1], '~', UDirectory::homeDir());

	std::list<std::string> databases = uSplit(inputDatabasePath, ';');
	if (databases.empty())
	{
		printf("No input database \"%s\" detected!\n", inputDatabasePath.c_str());
		return -1;
	}
	for (std::list<std::string>::iterator iter = databases.begin(); iter != databases.end(); ++iter)
	{
		if (!UFile::exists(*iter))
		{
			printf("Input database \"%s\" doesn't exist!\n", iter->c_str());
			return -1;
		}

		if (UFile::getExtension(*iter).compare("db") != 0)
		{
			printf("File \"%s\" is not a database format (*.db)!\n", iter->c_str());
			return -1;
		}
	}

	if(UFile::getExtension(outputDatabasePath).compare("db") != 0)
	{
		printf("File \"%s\" is not a database format (*.db)!\n", outputDatabasePath.c_str());
		return -1;
	}

	if(UFile::exists(outputDatabasePath))
	{
		UFile::erase(outputDatabasePath);
	}

	// Get parameters of the first database
	DBDriver * dbDriver = DBDriver::create();
	if(!dbDriver->openConnection(databases.front(), false))
	{
		printf("Failed opening input database!\n");
		delete dbDriver;
		return -1;
	}

	ParametersMap parameters = dbDriver->getLastParameters();
	std::string targetVersion = dbDriver->getDatabaseVersion();
	parameters.insert(ParametersPair(Parameters::kDbTargetVersion(), targetVersion));
	if(parameters.empty())
	{
		printf("WARNING: Failed getting parameters from database, reprocessing will be done with default parameters! Database version may be too old (%s).\n", dbDriver->getDatabaseVersion().c_str());
	}
	if(customParameters.size())
	{
		printf("Custom parameters:\n");
		for(ParametersMap::iterator iter=customParameters.begin(); iter!=customParameters.end(); ++iter)
		{
			printf("  %s\t= %s\n", iter->first.c_str(), iter->second.c_str());
		}
	}
	if((configParameters.find(Parameters::kKpDetectorStrategy())!=configParameters.end() ||
		configParameters.find(Parameters::kVisFeatureType())!=configParameters.end() ||
		customParameters.find(Parameters::kKpDetectorStrategy())!=customParameters.end() ||
		customParameters.find(Parameters::kVisFeatureType())!=customParameters.end()) &&
			configParameters.find(Parameters::kMemUseOdomFeatures())==configParameters.end() &&
			customParameters.find(Parameters::kMemUseOdomFeatures())==customParameters.end())
	{
		bool useOdomFeatures = Parameters::defaultMemUseOdomFeatures();
		Parameters::parse(parameters, Parameters::kMemUseOdomFeatures(), useOdomFeatures);
		if(useOdomFeatures)
		{
			printf("[Warning] %s and/or %s are overwritten but parameter %s is true in the opened database. "
					"Setting it to false for convenience to use the new selected feature detector. Set %s "
					"explicitly to suppress this warning.\n",
					Parameters::kKpDetectorStrategy().c_str(),
					Parameters::kVisFeatureType().c_str(),
					Parameters::kMemUseOdomFeatures().c_str(),
					Parameters::kMemUseOdomFeatures().c_str());
			uInsert(parameters, ParametersPair(Parameters::kMemUseOdomFeatures(), "false"));
		}
	}
	uInsert(parameters, configParameters);
	uInsert(parameters, customParameters);

	bool incrementalMemory = Parameters::defaultMemIncrementalMemory();
	Parameters::parse(parameters, Parameters::kMemIncrementalMemory(), incrementalMemory);
	Parameters::parse(parameters, Parameters::kDbTargetVersion(), targetVersion);
	bool intermediateNodes = Parameters::defaultRtabmapCreateIntermediateNodes();
	Parameters::parse(parameters, Parameters::kRtabmapCreateIntermediateNodes(), intermediateNodes);

	int totalIds = 0;
	std::set<int> ids;
	dbDriver->getAllNodeIds(ids, false, false, !intermediateNodes);
	if(ids.empty())
	{
		printf("Input database doesn't have any nodes saved in it.\n");
		dbDriver->closeConnection(false);
		delete dbDriver;
		return -1;
	}
	if(!(!incrementalMemory && databases.size() > 1))
	{
		totalIds = ids.size();
	}
	dbDriver->closeConnection(false);

	// Count remaining ids in the other databases
	for (std::list<std::string>::iterator iter = ++databases.begin(); iter != databases.end(); ++iter)
	{
		if (!dbDriver->openConnection(*iter, false))
		{
			printf("Failed opening input database!\n");
			delete dbDriver;
			return -1;
		}
		ids.clear();
		dbDriver->getAllNodeIds(ids, false, false, !intermediateNodes);
		totalIds += ids.size();
		dbDriver->closeConnection(false);
	}
	delete dbDriver;
	dbDriver = 0;

	if(framesToSkip)
	{
		totalIds/=framesToSkip+1;
	}

	std::string workingDirectory = UDirectory::getDir(outputDatabasePath);
	printf("Set working directory to \"%s\".\n", workingDirectory.c_str());
	if(!targetVersion.empty())
	{
		printf("Target database version: \"%s\" (set explicitly --%s \"\" to output with latest version.\n", targetVersion.c_str(), Parameters::kDbTargetVersion().c_str());
	}
	uInsert(parameters, ParametersPair(Parameters::kRtabmapWorkingDirectory(), workingDirectory));
	uInsert(parameters, ParametersPair(Parameters::kRtabmapPublishStats(), "true")); // to log status below

	if(!incrementalMemory && databases.size() > 1)
	{
		UFile::copy(databases.front(), outputDatabasePath);
		printf("Parameter \"%s\" is set to false, initializing RTAB-Map with \"%s\" for localization...\n", Parameters::kMemIncrementalMemory().c_str(), databases.front().c_str());
		databases.pop_front();
		inputDatabasePath = uJoin(databases, ";");
	}

	Rtabmap rtabmap;
	rtabmap.init(parameters, outputDatabasePath);

	bool rgbdEnabled = Parameters::defaultRGBDEnabled();
	Parameters::parse(parameters, Parameters::kRGBDEnabled(), rgbdEnabled);
	bool odometryIgnored = !rgbdEnabled;
	DBReader * dbReader = new DBReader(inputDatabasePath, useDatabaseRate?-1:0, odometryIgnored, false, false, startId, -1, stopId, !intermediateNodes);
	dbReader->init();

	OccupancyGrid grid(parameters);
	grid.setCloudAssembling(assemble3dMap);
#ifdef RTABMAP_OCTOMAP
	OctoMap octomap(parameters);
#endif

	float linearUpdate = Parameters::defaultRGBDLinearUpdate();
	float angularUpdate = Parameters::defaultRGBDAngularUpdate();
	Parameters::parse(parameters, Parameters::kRGBDLinearUpdate(), linearUpdate);
	Parameters::parse(parameters, Parameters::kRGBDAngularUpdate(), angularUpdate);

	printf("Reprocessing data of \"%s\"...\n", inputDatabasePath.c_str());
	std::map<std::string, float> globalMapStats;
	int processed = 0;
	CameraInfo info;
	SensorData data = dbReader->takeImage(&info);
	CameraThread camThread(dbReader, parameters); // take ownership of dbReader
	camThread.setScanParameters(scanFromDepth, scanDecimation, scanRangeMin, scanRangeMax, scanVoxelSize, scanNormalK, scanNormalRadius);
	if(scanFromDepth)
	{
		data.setLaserScan(LaserScan());
	}
	camThread.postUpdate(&data, &info);
	Transform lastLocalizationOdomPose = info.odomPose;
	bool inMotion = true;
	while(data.isValid() && g_loopForever)
	{
		UTimer iterationTime;
		std::string status;
		if(!odometryIgnored && info.odomPose.isNull())
		{
			printf("Skipping node %d as it doesn't have odometry pose set.\n", data.id());
		}
		else
		{
			if(!odometryIgnored && !info.odomCovariance.empty() && info.odomCovariance.at<double>(0,0)>=9999)
			{
				printf("High variance detected, triggering a new map...\n");
				if(!incrementalMemory && processed>0)
				{
					showLocalizationStats(outputDatabasePath);
					lastLocalizationOdomPose = info.odomPose;
				}
				rtabmap.triggerNewMap();
				inMotion = true;
			}
			UTimer t;
			if(!rtabmap.process(data, info.odomPose, info.odomCovariance, info.odomVelocity, globalMapStats))
			{
				printf("Failed processing node %d.\n", data.id());
				globalMapStats.clear();
			}
			else if(assemble2dMap || assemble3dMap || assemble2dOctoMap || assemble3dOctoMap)
			{
				globalMapStats.clear();
				double timeRtabmap = t.ticks();
				double timeUpdateInit = 0.0;
				double timeUpdateGrid = 0.0;
#ifdef RTABMAP_OCTOMAP
				double timeUpdateOctoMap = 0.0;
#endif
				const rtabmap::Statistics & stats = rtabmap.getStatistics();
				if(stats.poses().size() && stats.getLastSignatureData().id())
				{
					int id = stats.poses().rbegin()->first;
					if(id == stats.getLastSignatureData().id() &&
					   stats.getLastSignatureData().sensorData().gridCellSize() > 0.0f)
					{
						bool updateGridMap = false;
						bool updateOctoMap = false;
						if((assemble2dMap || assemble3dMap) && grid.addedNodes().find(id) == grid.addedNodes().end())
						{
							updateGridMap = true;
						}
#ifdef RTABMAP_OCTOMAP
						if((assemble2dOctoMap || assemble3dOctoMap) && octomap.addedNodes().find(id) == octomap.addedNodes().end())
						{
							updateOctoMap = true;
						}
#endif
						if(updateGridMap || updateOctoMap)
						{
							cv::Mat ground, obstacles, empty;
							stats.getLastSignatureData().sensorData().uncompressDataConst(0, 0, 0, 0, &ground, &obstacles, &empty);

							timeUpdateInit = t.ticks();

							if(updateGridMap)
							{
								grid.addToCache(id, ground, obstacles, empty);
								grid.update(stats.poses());
								timeUpdateGrid = t.ticks() + timeUpdateInit;
							}
#ifdef RTABMAP_OCTOMAP
							if(updateOctoMap)
							{
								const cv::Point3f & viewpoint = stats.getLastSignatureData().sensorData().gridViewPoint();
								octomap.addToCache(id, ground, obstacles, empty, viewpoint);
								octomap.update(stats.poses());
								timeUpdateOctoMap = t.ticks() + timeUpdateInit;
							}
#endif
						}
					}
				}

				globalMapStats.insert(std::make_pair(std::string("GlobalGrid/GridUpdate/ms"), timeUpdateGrid*1000.0f));
#ifdef RTABMAP_OCTOMAP
				//Simulate publishing
				double timePub2dOctoMap = 0.0;
				double timePub3dOctoMap = 0.0;
				if(assemble2dOctoMap)
				{
					float xMin, yMin, size;
					octomap.createProjectionMap(xMin, yMin, size);
					timePub2dOctoMap = t.ticks();
				}
				if(assemble3dOctoMap)
				{
					octomap.createCloud();
					timePub3dOctoMap = t.ticks();
				}

				globalMapStats.insert(std::make_pair(std::string("GlobalGrid/OctoMapUpdate/ms"), timeUpdateOctoMap*1000.0f));
				globalMapStats.insert(std::make_pair(std::string("GlobalGrid/OctoMapProjection/ms"), timePub2dOctoMap*1000.0f));
				globalMapStats.insert(std::make_pair(std::string("GlobalGrid/OctomapToCloud/ms"), timePub3dOctoMap*1000.0f));
				globalMapStats.insert(std::make_pair(std::string("GlobalGrid/TotalWithRtabmap/ms"), (timeUpdateGrid+timeUpdateOctoMap+timePub2dOctoMap+timePub3dOctoMap+timeRtabmap)*1000.0f));
#else
				globalMapStats.insert(std::make_pair(std::string("GlobalGrid/TotalWithRtabmap/ms"), (timeUpdateGrid+timeRtabmap)*1000.0f));
#endif
			}
		}

		const rtabmap::Statistics & stats = rtabmap.getStatistics();
		int refId = stats.refImageId();
		int loopId = stats.loopClosureId() > 0? stats.loopClosureId(): stats.proximityDetectionId() > 0?stats.proximityDetectionId() :0;
		int landmarkId = (int)uValue(stats.data(), rtabmap::Statistics::kLoopLandmark_detected(), 0.0f);
		int refMapId = stats.refImageMapId();
		++totalFrames;

		if(inMotion)
		{
			++totalFramesMotion;
		}
		if (loopId>0)
		{
			if(stats.loopClosureId()>0)
			{
				++loopCount;
			}
			else
			{
				++proxCount;
			}
			if(inMotion)
			{
				++loopCountMotion;
			}
			int loopMapId = stats.loopClosureId() > 0? stats.loopClosureMapId(): stats.proximityDetectionMapId();
			printf("Processed %d/%d nodes [id=%d map=%d]... %dms %s on %d [%d]\n", ++processed, totalIds, refId, refMapId, int(iterationTime.ticks() * 1000), stats.loopClosureId() > 0?"Loop":"Prox", loopId, loopMapId);
		}
		else if(landmarkId != 0)
		{
			printf("Processed %d/%d nodes [id=%d map=%d]... %dms Loop on landmark %d\n", ++processed, totalIds, refId, refMapId, int(iterationTime.ticks() * 1000), landmarkId);
		}
		else
		{
			printf("Processed %d/%d nodes [id=%d map=%d]... %dms\n", ++processed, totalIds, refId, refMapId, int(iterationTime.ticks() * 1000));
		}

		// Here we accumulate statistics about distance from last localization
		if(!incrementalMemory &&
		   !lastLocalizationOdomPose.isNull() &&
		   !info.odomPose.isNull())
		{
			if(loopId>0 || landmarkId != 0)
			{
				previousLocalizationDistances.push_back(lastLocalizationOdomPose.getDistance(info.odomPose));
				lastLocalizationOdomPose = info.odomPose;
			}
		}
		if(!incrementalMemory)
		{
			float totalTime = uValue(stats.data(), rtabmap::Statistics::kTimingTotal(), 0.0f);
			localizationTime.push_back(totalTime);
			if(stats.data().find(Statistics::kLoopOdom_correction_norm()) != stats.data().end())
			{
				localizationVariations.push_back(stats.data().at(Statistics::kLoopOdom_correction_norm()));
				localizationAngleVariations.push_back(stats.data().at(Statistics::kLoopOdom_correction_angle()));
			}

			if(exportPoses && !info.odomPose.isNull())
			{
				if(!odomTrajectoryPoses.empty())
				{
					int previousId = odomTrajectoryPoses.rbegin()->first;
					odomTrajectoryLinks.insert(std::make_pair(previousId, Link(previousId, refId, Link::kNeighbor, odomTrajectoryPoses.rbegin()->second.inverse()*info.odomPose, info.odomCovariance)));
				}
				odomTrajectoryPoses.insert(std::make_pair(refId, info.odomPose));
				localizationPoses.insert(std::make_pair(refId, stats.mapCorrection()*info.odomPose));
			}
		}

		Transform odomPose = info.odomPose;

		if(framesToSkip>0)
		{
			int skippedFrames = framesToSkip;
			while(skippedFrames-- > 0)
			{
				data = dbReader->takeImage(&info);
				if(!odometryIgnored && !info.odomCovariance.empty() && info.odomCovariance.at<double>(0,0)>=9999)
				{
					printf("High variance detected, triggering a new map...\n");
					if(!incrementalMemory && processed>0)
					{
						showLocalizationStats(outputDatabasePath);
						lastLocalizationOdomPose = info.odomPose;
					}
					rtabmap.triggerNewMap();
				}
			}
		}

		data = dbReader->takeImage(&info);
		if(scanFromDepth)
		{
			data.setLaserScan(LaserScan());
		}
		camThread.postUpdate(&data, &info);

		inMotion = true;
		if(!incrementalMemory &&
		   !odomPose.isNull() &&
		   !info.odomPose.isNull())
		{
			float distance = odomPose.getDistance(info.odomPose);
			float angle = (odomPose.inverse()*info.odomPose).getAngle();
			odomDistances.push_back(distance);
			if(distance < linearUpdate && angle <= angularUpdate)
			{
				inMotion = false;
			}
		}
	}

	int databasesMerged = 0;
	if(!incrementalMemory)
	{
		showLocalizationStats(outputDatabasePath);
	}
	else
	{
		printf("Total loop closures = %d (Loop=%d, Prox=%d, In Motion=%d/%d)\n", loopCount+proxCount, loopCount, proxCount, loopCountMotion, totalFramesMotion);

		if(databases.size()>1)
		{
			std::map<int, Transform> poses;
			std::multimap<int, Link> constraints;
			rtabmap.getGraph(poses, constraints, 0, 1, 0, false, false, false, false, false, false);
			std::set<int> mapIds;
			for(std::map<int, Transform>::iterator iter=poses.begin(); iter!=poses.end(); ++iter)
			{
				int id;
				if((id=rtabmap.getMemory()->getMapId(iter->first, true))>=0)
				{
					mapIds.insert(id);
				}
			}
			databasesMerged = mapIds.size();
		}
	}

	printf("Closing database \"%s\"...\n", outputDatabasePath.c_str());
	rtabmap.close(true);
	printf("Closing database \"%s\"... done!\n", outputDatabasePath.c_str());

	if(assemble2dMap)
	{
		std::string outputPath = outputDatabasePath.substr(0, outputDatabasePath.size()-3) + "_map.pgm";
		float xMin,yMin;
		cv::Mat map = grid.getMap(xMin, yMin);
		if(!map.empty())
		{
			if(save2DMap)
			{
				DBDriver * driver = DBDriver::create();
				if(driver->openConnection(outputDatabasePath))
				{
					driver->save2DMap(map, xMin, yMin, grid.getCellSize());
					printf("Saving occupancy grid to database... done!\n");
				}
				delete driver;
			}
			else
			{
				cv::Mat map8U(map.rows, map.cols, CV_8U);
				//convert to gray scaled map
				for (int i = 0; i < map.rows; ++i)
				{
					for (int j = 0; j < map.cols; ++j)
					{
						char v = map.at<char>(i, j);
						unsigned char gray;
						if(v == 0)
						{
							gray = 178;
						}
						else if(v == 100)
						{
							gray = 0;
						}
						else // -1
						{
							gray = 89;
						}
						map8U.at<unsigned char>(i, j) = gray;
					}
				}

				if(cv::imwrite(outputPath, map8U))
				{
					printf("Saving occupancy grid \"%s\"... done!\n", outputPath.c_str());
				}
				else
				{
					printf("Saving occupancy grid \"%s\"... failed!\n", outputPath.c_str());
				}
			}
		}
		else
		{
			printf("2D map is empty! Cannot save it!\n");
		}
	}
	if(assemble3dMap)
	{
		std::string outputPath = outputDatabasePath.substr(0, outputDatabasePath.size()-3) + "_obstacles.pcd";
		if(pcl::io::savePCDFileBinary(outputPath, *grid.getMapObstacles()) == 0)
		{
			printf("Saving 3d obstacles \"%s\"... done!\n", outputPath.c_str());
		}
		else
		{
			printf("Saving 3d obstacles \"%s\"... failed!\n", outputPath.c_str());
		}
		if(grid.getMapGround()->size())
		{
			outputPath = outputDatabasePath.substr(0, outputDatabasePath.size()-3) + "_ground.pcd";
			if(pcl::io::savePCDFileBinary(outputPath, *grid.getMapGround()) == 0)
			{
				printf("Saving 3d ground \"%s\"... done!\n", outputPath.c_str());
			}
			else
			{
				printf("Saving 3d ground \"%s\"... failed!\n", outputPath.c_str());
			}
		}
		if(grid.getMapEmptyCells()->size())
		{
			outputPath = outputDatabasePath.substr(0, outputDatabasePath.size()-3) + "_empty.pcd";
			if(pcl::io::savePCDFileBinary(outputPath, *grid.getMapEmptyCells()) == 0)
			{
				printf("Saving 3d empty cells \"%s\"... done!\n", outputPath.c_str());
			}
			else
			{
				printf("Saving 3d empty cells \"%s\"... failed!\n", outputPath.c_str());
			}
		}
	}
#ifdef RTABMAP_OCTOMAP
	if(assemble2dOctoMap)
	{
		std::string outputPath = outputDatabasePath.substr(0, outputDatabasePath.size()-3) + "_octomap.pgm";
		float xMin,yMin,cellSize;
		cv::Mat map = octomap.createProjectionMap(xMin, yMin, cellSize);
		if(!map.empty())
		{
			if(save2DMap)
			{
				DBDriver * driver = DBDriver::create();
				if(driver->openConnection(outputDatabasePath))
				{
					driver->save2DMap(map, xMin, yMin, cellSize);
					printf("Saving occupancy grid to database... done!\n");
				}
				delete driver;
			}
			else
			{
				cv::Mat map8U(map.rows, map.cols, CV_8U);
				//convert to gray scaled map
				for (int i = 0; i < map.rows; ++i)
				{
					for (int j = 0; j < map.cols; ++j)
					{
						char v = map.at<char>(i, j);
						unsigned char gray;
						if(v == 0)
						{
							gray = 178;
						}
						else if(v == 100)
						{
							gray = 0;
						}
						else // -1
						{
							gray = 89;
						}
						map8U.at<unsigned char>(i, j) = gray;
					}
				}
				if(cv::imwrite(outputPath, map8U))
				{
					printf("Saving octomap 2D projection \"%s\"... done!\n", outputPath.c_str());
				}
				else
				{
					printf("Saving octomap 2D projection \"%s\"... failed!\n", outputPath.c_str());
				}
			}
		}
		else
		{
			printf("OctoMap 2D projection map is empty! Cannot save it!\n");
		}
	}
	if(assemble3dOctoMap)
	{
		std::string outputPath = outputDatabasePath.substr(0, outputDatabasePath.size()-3) + "_octomap_occupied.pcd";
		std::vector<int> obstacles, emptySpace, ground;
		pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud = octomap.createCloud(0, &obstacles, &emptySpace, &ground);
		if(pcl::io::savePCDFile(outputPath, *cloud, obstacles, true) == 0)
		{
			printf("Saving obstacles cloud \"%s\"... done!\n", outputPath.c_str());
		}
		else
		{
			printf("Saving obstacles cloud \"%s\"... failed!\n", outputPath.c_str());
		}
		if(ground.size())
		{
			outputPath = outputDatabasePath.substr(0, outputDatabasePath.size()-3) + "_octomap_ground.pcd";
			if(pcl::io::savePCDFile(outputPath, *cloud, ground, true) == 0)
			{
				printf("Saving empty space cloud \"%s\"... done!\n", outputPath.c_str());
			}
			else
			{
				printf("Saving empty space cloud \"%s\"... failed!\n", outputPath.c_str());
			}
		}
		if(emptySpace.size())
		{
			outputPath = outputDatabasePath.substr(0, outputDatabasePath.size()-3) + "_octomap_empty.pcd";
			if(pcl::io::savePCDFile(outputPath, *cloud, emptySpace, true) == 0)
			{
				printf("Saving empty space cloud \"%s\"... done!\n", outputPath.c_str());
			}
			else
			{
				printf("Saving empty space cloud \"%s\"... failed!\n", outputPath.c_str());
			}
		}
	}
#endif

	return databasesMerged;
}
