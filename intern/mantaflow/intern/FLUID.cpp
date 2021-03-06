/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2016 Blender Foundation.
 * All rights reserved.
 *
 * Contributor(s): Sebastian Barschkis (sebbas)
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file mantaflow/intern/FLUID.cpp
 *  \ingroup mantaflow
 */

#include <sstream>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <zlib.h>

#include "FLUID.h"
#include "manta.h"
#include "Python.h"
#include "shared_script.h"
#include "smoke_script.h"
#include "liquid_script.h"

#include "BLI_path_util.h"
#include "BLI_utildefines.h"
#include "BLI_fileops.h"

#include "DNA_scene_types.h"
#include "DNA_modifier_types.h"
#include "DNA_smoke_types.h"

std::atomic<bool> FLUID::mantaInitialized(false);
std::atomic<int> FLUID::solverID(0);
int FLUID::with_debug(0);

FLUID::FLUID(int *res, SmokeModifierData *smd) : mCurrentID(++solverID)
{
	if (with_debug)
		std::cout << "FLUID: " << mCurrentID << " with res(" << res[0] << ", " << res[1] << ", " << res[2] << ")" << std::endl;

	smd->domain->fluid = this;
	
	mUsingHeat     = smd->domain->active_fields & SM_ACTIVE_HEAT;
	mUsingFire     = smd->domain->active_fields & SM_ACTIVE_FIRE;
	mUsingColors   = smd->domain->active_fields & SM_ACTIVE_COLORS;
	mUsingObstacle = smd->domain->active_fields & SM_ACTIVE_OBSTACLE;
	mUsingGuiding  = smd->domain->active_fields & SM_ACTIVE_GUIDING;
	mUsingInvel    = smd->domain->active_fields & SM_ACTIVE_INVEL;
	mUsingNoise    = smd->domain->flags & MOD_SMOKE_NOISE;
	mUsingMesh     = smd->domain->flags & MOD_SMOKE_MESH;
	mUsingLiquid   = smd->domain->type == MOD_SMOKE_DOMAIN_TYPE_LIQUID;
	mUsingSmoke    = smd->domain->type == MOD_SMOKE_DOMAIN_TYPE_GAS;
	mUsingDrops    = smd->domain->particle_type & MOD_SMOKE_PARTICLE_DROP;
	mUsingBubbles  = smd->domain->particle_type & MOD_SMOKE_PARTICLE_BUBBLE;
	mUsingFloats   = smd->domain->particle_type & MOD_SMOKE_PARTICLE_FLOAT;
	mUsingTracers  = smd->domain->particle_type & MOD_SMOKE_PARTICLE_TRACER;

	// Simulation constants
	mTempAmb            = 0; // TODO: Maybe use this later for buoyancy calculation
	mResX               = res[0];
	mResY               = res[1];
	mResZ               = res[2];
	mMaxRes             = MAX3(mResX, mResY, mResZ);
	mConstantScaling    = 64.0f / mMaxRes;
	mConstantScaling    = (mConstantScaling < 1.0f) ? 1.0f : mConstantScaling;
	mTotalCells         = mResX * mResY * mResZ;

	// Smoke low res grids
	mDensity        = NULL;
	mEmissionIn     = NULL;
	mShadow         = NULL;
	mFlowType       = NULL;
	mNumFlow        = NULL;
	mHeat           = NULL;
	mVelocityX      = NULL;
	mVelocityY      = NULL;
	mVelocityZ      = NULL;
	mForceX         = NULL;
	mForceY         = NULL;
	mForceZ         = NULL;
	mFlame          = NULL;
	mFuel           = NULL;
	mReact          = NULL;
	mColorR         = NULL;
	mColorG         = NULL;
	mColorB         = NULL;
	mObstacle       = NULL;

	// Smoke high res grids
	mDensityHigh    = NULL;
	mFlameHigh      = NULL;
	mFuelHigh       = NULL;
	mReactHigh      = NULL;
	mColorRHigh     = NULL;
	mColorGHigh     = NULL;
	mColorBHigh     = NULL;
	mTextureU       = NULL;
	mTextureV       = NULL;
	mTextureW       = NULL;
	mTextureU2      = NULL;
	mTextureV2      = NULL;
	mTextureW2      = NULL;

	// Liquid low res grids
	mPhiIn          = NULL;
	mPhiOutIn       = NULL;
	mPhi            = NULL;

	// Mesh
	mMeshNodes      = NULL;
	mMeshTriangles  = NULL;

	// Fluid obstacle
	mPhiObsIn    = NULL;
	mNumObstacle = NULL;
	mObVelocityX = NULL;
	mObVelocityY = NULL;
	mObVelocityZ = NULL;

	// Fluid guiding
	mPhiGuideIn     = NULL;
	mNumGuide       = NULL;
	mGuideVelocityX = NULL;
	mGuideVelocityY = NULL;
	mGuideVelocityZ = NULL;

	// Fluid initial velocity
	mInVelocityX    = NULL;
	mInVelocityY    = NULL;
	mInVelocityZ    = NULL;

	// Secondary particles
	mFlipParticleData      = NULL;
	mFlipParticleVelocity  = NULL;
	mSndParticleData       = NULL;
	mSndParticleVelocity   = NULL;
	mSndParticleLife       = NULL;

	// Only start Mantaflow once. No need to start whenever new FLUID objected is allocated
	if (!mantaInitialized)
		initializeMantaflow();

	// Initialize Mantaflow variables in Python
	// Liquid
	if (mUsingLiquid) {
		initDomain(smd);
		initLiquid(smd);
		if (mUsingObstacle) initObstacle(smd);
		if (mUsingGuiding)  initGuiding(smd);
		if (mUsingInvel)    initInVelocity(smd);

		if (mUsingDrops || mUsingBubbles || mUsingFloats || mUsingTracers) {
			mUpresParticle       = smd->domain->particle_scale;
			mResXParticle        = mUpresParticle * mResX;
			mResYParticle        = mUpresParticle * mResY;
			mResZParticle        = mUpresParticle * mResZ;
			mTotalCellsParticles = mResXParticle * mResYParticle * mResZParticle;
	
			initSndParts(smd);
			initLiquidSndParts(smd);
		}
		
		if (mUsingMesh) {
			mUpresMesh      = smd->domain->mesh_scale;
			mResXMesh       = mUpresMesh * mResX;
			mResYMesh       = mUpresMesh * mResY;
			mResZMesh       = mUpresMesh * mResZ;
			mTotalCellsMesh	= mResXMesh * mResYMesh * mResZMesh;

			// Initialize Mantaflow variables in Python
			initMesh(smd);
			initLiquidMesh(smd);
		}
		updatePointers();

		return;
	}
	
	// Smoke
	if (mUsingSmoke) {
		initDomain(smd);
		initSmoke(smd);
		if (mUsingHeat)     initHeat(smd);
		if (mUsingFire)     initFire(smd);
		if (mUsingColors)   initColors(smd);
		if (mUsingObstacle) initObstacle(smd);
		if (mUsingGuiding)  initGuiding(smd);
		if (mUsingInvel)    initInVelocity(smd);

		updatePointers(); // Needs to be after heat, fire, color init

		if (mUsingNoise) {
			int amplify     = smd->domain->noise_scale;
			mResXNoise      = amplify * mResX;
			mResYNoise      = amplify * mResY;
			mResZNoise      = amplify * mResZ;
			mTotalCellsHigh	= mResXNoise * mResYNoise * mResZNoise;
			
			// Initialize Mantaflow variables in Python
			initNoise(smd);
			initSmokeNoise(smd);
			if (mUsingFire)   initFireHigh(smd);
			if (mUsingColors) initColorsHigh(smd);

			updatePointersHigh(); // Needs to be after fire, color init
		}
	}
}

void FLUID::initDomain(SmokeModifierData *smd)
{
	// Vector will hold all python commands that are to be executed
	std::vector<std::string> pythonCommands;

	// Set manta debug level first
	pythonCommands.push_back(manta_import + manta_debuglevel);

	std::ostringstream ss;
	ss <<  "set_manta_debuglevel(" << with_debug << ")";
	pythonCommands.push_back(ss.str());

	// Now init basic fluid domain
	std::string tmpString = fluid_variables
		+ fluid_solver
		+ fluid_alloc
		+ fluid_bake_helper
		+ fluid_bake_data
		+ fluid_bake_noise
		+ fluid_bake_mesh
		+ fluid_bake_particles
		+ fluid_file_import
		+ fluid_file_export
		+ fluid_save_data
		+ fluid_load_data
		+ fluid_adapt_time_step
		+ fluid_adaptive_time_stepping;
	std::string finalString = parseScript(tmpString, smd);
	pythonCommands.push_back(finalString);
	runPythonString(pythonCommands);
}

void FLUID::initNoise(SmokeModifierData *smd)
{
	std::vector<std::string> pythonCommands;
	std::string tmpString = fluid_variables_noise
		+ fluid_solver_noise
		+ fluid_adapt_time_step_noise
		+ fluid_adaptive_time_stepping_noise;
	std::string finalString = parseScript(tmpString, smd);
	pythonCommands.push_back(finalString);
	
	runPythonString(pythonCommands);
}

void FLUID::initSmoke(SmokeModifierData *smd)
{
	std::vector<std::string> pythonCommands;
	std::string tmpString = smoke_alloc
		+ smoke_variables
		+ smoke_bounds
		+ smoke_adaptive_step
		+ smoke_save_data
		+ smoke_load_data
		+ smoke_pre_step
		+ smoke_step
		+ smoke_post_step;
	std::string finalString = parseScript(tmpString, smd);
	pythonCommands.push_back(finalString);
	
	runPythonString(pythonCommands);
}

void FLUID::initSmokeNoise(SmokeModifierData *smd)
{
	std::vector<std::string> pythonCommands;
	std::string tmpString = smoke_alloc_noise
		+ smoke_wavelet_turbulence_noise
		+ smoke_variables_noise
		+ smoke_bounds_noise
		+ smoke_adaptive_step_noise
		+ smoke_save_noise
		+ smoke_load_noise
		+ smoke_pre_step_noise
		+ smoke_step_noise
		+ smoke_post_step_noise;
	std::string finalString = parseScript(tmpString, smd);
	pythonCommands.push_back(finalString);

	runPythonString(pythonCommands);
	mUsingNoise = true;
}

void FLUID::initHeat(SmokeModifierData *smd)
{
	if (!mHeat) {
		std::vector<std::string> pythonCommands;
		std::string tmpString = smoke_alloc_heat_low
			+ smoke_with_heat;
		std::string finalString = parseScript(tmpString, smd);
		pythonCommands.push_back(finalString);
		
		runPythonString(pythonCommands);
		mUsingHeat = true;
	}
}

void FLUID::initFire(SmokeModifierData *smd)
{
	if (!mFuel) {
		std::vector<std::string> pythonCommands;
		std::string tmpString = smoke_alloc_fire_low
			+ smoke_with_fire;
		std::string finalString = parseScript(tmpString, smd);
		pythonCommands.push_back(finalString);

		runPythonString(pythonCommands);
		mUsingFire = true;
	}
}

void FLUID::initFireHigh(SmokeModifierData *smd)
{
	if (!mFuelHigh) {
		std::vector<std::string> pythonCommands;
		std::string tmpString = smoke_alloc_fire_high
			+ smoke_with_fire;
		std::string finalString = parseScript(tmpString, smd);
		pythonCommands.push_back(finalString);

		runPythonString(pythonCommands);
		mUsingFire = true;
	}
}

void FLUID::initColors(SmokeModifierData *smd)
{
	if (!mColorR) {
		std::vector<std::string> pythonCommands;
		std::string tmpString = smoke_alloc_colors_low
			+ smoke_init_colors_low
			+ smoke_with_colors;
		std::string finalString = parseScript(tmpString, smd);
		pythonCommands.push_back(finalString);

		runPythonString(pythonCommands);
		mUsingColors = true;
	}
}

void FLUID::initColorsHigh(SmokeModifierData *smd)
{
	if (!mColorRHigh) {
		std::vector<std::string> pythonCommands;
		std::string tmpString = smoke_alloc_colors_high
			+ smoke_init_colors_high
			+ smoke_with_colors;
		std::string finalString = parseScript(tmpString, smd);
		pythonCommands.push_back(finalString);

		runPythonString(pythonCommands);
		mUsingColors = true;
	}
}

void FLUID::initLiquid(SmokeModifierData *smd)
{
	if (!mPhiIn) {
		std::vector<std::string> pythonCommands;
		std::string tmpString = liquid_alloc
			+ liquid_variables
			+ liquid_init_phi
			+ liquid_save_data
			+ liquid_save_flip
			+ liquid_load_data
			+ liquid_load_flip
			+ liquid_adaptive_step
			+ liquid_pre_step
			+ liquid_step
			+ liquid_post_step
			+ liquid_step_particles;
		std::string finalString = parseScript(tmpString, smd);
		pythonCommands.push_back(finalString);

		runPythonString(pythonCommands);
		mUsingLiquid = true;
	}
}

void FLUID::initMesh(SmokeModifierData *smd)
{
	std::vector<std::string> pythonCommands;
	std::string tmpString = fluid_variables_mesh
		+ fluid_solver_mesh;
	std::string finalString = parseScript(tmpString, smd);
	pythonCommands.push_back(finalString);

	runPythonString(pythonCommands);
	mUsingMesh = true;
}

void FLUID::initLiquidMesh(SmokeModifierData *smd)
{
	std::vector<std::string> pythonCommands;
	std::string tmpString = liquid_alloc_mesh
		+ liquid_step_mesh
		+ liquid_save_mesh;
	std::string finalString = parseScript(tmpString, smd);
	pythonCommands.push_back(finalString);

	runPythonString(pythonCommands);
	mUsingMesh = true;
}

void FLUID::initObstacle(SmokeModifierData *smd)
{
	if (!mPhiObsIn) {
		std::vector<std::string> pythonCommands;
		std::string tmpString = fluid_alloc_obstacle_low
			+ fluid_with_obstacle;
		std::string finalString = parseScript(tmpString, smd);
		pythonCommands.push_back(finalString);

		runPythonString(pythonCommands);
		mUsingObstacle = true;
	}
}

void FLUID::initGuiding(SmokeModifierData *smd)
{
	if (!mPhiGuideIn) {
		std::vector<std::string> pythonCommands;
		std::string tmpString = fluid_alloc_guiding_low
			+ fluid_with_guiding;
		std::string finalString = parseScript(tmpString, smd);
		pythonCommands.push_back(finalString);

		runPythonString(pythonCommands);
		mUsingGuiding = true;
	}
}

void FLUID::initInVelocity(SmokeModifierData *smd)
{
	if (!mInVelocityX) {
		std::vector<std::string> pythonCommands;
		std::string tmpString = fluid_alloc_invel_low
			+ fluid_with_invel;
		std::string finalString = parseScript(tmpString, smd);
		pythonCommands.push_back(finalString);

		runPythonString(pythonCommands);
		mUsingInvel = true;
	}
}

void FLUID::initSndParts(SmokeModifierData *smd)
{
	std::vector<std::string> pythonCommands;
	std::string tmpString = fluid_variables_particles
		+ fluid_solver_particles
		+ fluid_load_particles
		+ fluid_save_particles;
	std::string finalString = parseScript(tmpString, smd);
	pythonCommands.push_back(finalString);

	runPythonString(pythonCommands);
}

void FLUID::initLiquidSndParts(SmokeModifierData *smd)
{
	if (!mSndParticleData) {
		std::vector<std::string> pythonCommands;
		std::string tmpString = fluid_alloc_sndparts
			+ fluid_with_sndparts;
		std::string finalString = parseScript(tmpString, smd);
		pythonCommands.push_back(finalString);

		runPythonString(pythonCommands);
	}
}

FLUID::~FLUID()
{
	if (with_debug)
		std::cout << "~FLUID: " << mCurrentID << " with res(" << mResX << ", " << mResY << ", " << mResZ << ")" << std::endl;

	// Destruction string for Python
	std::string tmpString = "";
	std::vector<std::string> pythonCommands;

	tmpString += manta_import;
	tmpString += fluid_delete_all;

	// Safe to pass NULL argument since only looking up IDs
	std::string finalString = parseScript(tmpString, NULL);
	pythonCommands.push_back(finalString);
	runPythonString(pythonCommands);

	// Reset pointers to avoid dangling pointers
	mDensity        = NULL;
	mEmissionIn     = NULL;
	mShadow         = NULL;
	mFlowType       = NULL;
	mNumFlow        = NULL;
	mHeat           = NULL;
	mVelocityX      = NULL;
	mVelocityY      = NULL;
	mVelocityZ      = NULL;
	mForceX         = NULL;
	mForceY         = NULL;
	mForceZ         = NULL;
	mFlame          = NULL;
	mFuel           = NULL;
	mReact          = NULL;
	mColorR         = NULL;
	mColorG         = NULL;
	mColorB         = NULL;
	mObstacle       = NULL;

	mDensityHigh    = NULL;
	mFlameHigh      = NULL;
	mFuelHigh       = NULL;
	mReactHigh      = NULL;
	mColorRHigh     = NULL;
	mColorGHigh     = NULL;
	mColorBHigh     = NULL;
	mTextureU       = NULL;
	mTextureV       = NULL;
	mTextureW       = NULL;
	mTextureU2      = NULL;
	mTextureV2      = NULL;
	mTextureW2      = NULL;

	// Liquid
	mPhiIn      = NULL;
	mPhiOutIn   = NULL;
	mPhi        = NULL;

	// Mesh
	mMeshNodes      = NULL;
	mMeshTriangles  = NULL;

	// Fluid obstacle
	mPhiObsIn    = NULL;
	mNumObstacle = NULL;
	mObVelocityX = NULL;
	mObVelocityY = NULL;
	mObVelocityZ = NULL;

	// Fluid guiding
	mPhiGuideIn     = NULL;
	mNumGuide       = NULL;
	mGuideVelocityX = NULL;
	mGuideVelocityY = NULL;
	mGuideVelocityZ = NULL;

	// Fluid initial velocity
	mInVelocityX    = NULL;
	mInVelocityY    = NULL;
	mInVelocityZ    = NULL;

	// Secondary particles
	mFlipParticleData      = NULL;
	mFlipParticleVelocity  = NULL;
	mSndParticleData       = NULL;
	mSndParticleVelocity   = NULL;
	mSndParticleLife       = NULL;

	// Reset flags
	mUsingHeat     = false;
	mUsingFire     = false;
	mUsingColors   = false;
	mUsingObstacle = false;
	mUsingGuiding  = false;
	mUsingInvel    = false;
	mUsingNoise    = false;
	mUsingMesh     = false;
}

void FLUID::runPythonString(std::vector<std::string> commands)
{
	PyGILState_STATE gilstate = PyGILState_Ensure();
	for (std::vector<std::string>::iterator it = commands.begin(); it != commands.end(); ++it) {
		std::string command = *it;

#ifdef WIN32
		// special treatment for windows when running python code
		size_t cmdLength = command.length();
		char* buffer = new char[cmdLength+1];
		memcpy(buffer, command.data(), cmdLength);

		buffer[cmdLength] = '\0';
		PyRun_SimpleString(buffer);
		delete[] buffer;
#else
		PyRun_SimpleString(command.c_str());
#endif
	}
	PyGILState_Release(gilstate);
}

void FLUID::initializeMantaflow()
{
	if (with_debug)
		std::cout << "Initializing  Mantaflow" << std::endl;

	std::string filename = "manta_scene_" + std::to_string(mCurrentID) + ".py";
	std::vector<std::string> fill = std::vector<std::string>();
	
	// Initialize extension classes and wrappers
	srand(0);
	PyGILState_STATE gilstate = PyGILState_Ensure();
	Pb::setup(filename, fill);  // Namespace from Mantaflow (registry)
	PyGILState_Release(gilstate);
	mantaInitialized = true;
}

void FLUID::terminateMantaflow()
{
	if (with_debug)
		std::cout << "Terminating Mantaflow" << std::endl;

	PyGILState_STATE gilstate = PyGILState_Ensure();
	Pb::finalize();  // Namespace from Mantaflow (registry)
	PyGILState_Release(gilstate);
	mantaInitialized = false;
}

std::string FLUID::getRealValue(const std::string& varName,  SmokeModifierData *smd)
{
	std::ostringstream ss;
	bool is2D = false;
	ModifierData *md;
	int tmpVar;
	
	if (smd) {
		is2D = (smd->domain->manta_solver_res == 2);
		md = ((ModifierData*) smd);
	}

	if (varName == "USING_SMOKE")
		ss << ((smd->domain->type == MOD_SMOKE_DOMAIN_TYPE_GAS) ? "True" : "False");
	else if (varName == "USING_LIQUID")
		ss << ((smd->domain->type == MOD_SMOKE_DOMAIN_TYPE_LIQUID) ? "True" : "False");
	else if (varName == "USING_COLORS")
		ss << (smd->domain->active_fields & SM_ACTIVE_COLORS ? "True" : "False");
	else if (varName == "USING_HEAT")
		ss << (smd->domain->active_fields & SM_ACTIVE_HEAT ? "True" : "False");
	else if (varName == "USING_FIRE")
		ss << (smd->domain->active_fields & SM_ACTIVE_FIRE ? "True" : "False");
	else if (varName == "USING_HIGHRES")
		ss << (smd->domain->flags & MOD_SMOKE_NOISE ? "True" : "False");
	else if (varName == "USING_OBSTACLE")
		ss << (smd->domain->active_fields & SM_ACTIVE_OBSTACLE ? "True" : "False");
	else if (varName == "USING_GUIDING")
		ss << (smd->domain->active_fields & SM_ACTIVE_GUIDING ? "True" : "False");
	else if (varName == "USING_INVEL")
		ss << (smd->domain->active_fields & SM_ACTIVE_INVEL ? "True" : "False");
	else if (varName == "SOLVER_DIM")
		ss << smd->domain->manta_solver_res;
	else if (varName == "DO_OPEN") {
		tmpVar = (MOD_SMOKE_BORDER_BACK | MOD_SMOKE_BORDER_FRONT |
				  MOD_SMOKE_BORDER_LEFT | MOD_SMOKE_BORDER_RIGHT |
				  MOD_SMOKE_BORDER_BOTTOM | MOD_SMOKE_BORDER_TOP);
		ss << (((smd->domain->border_collisions & tmpVar) == tmpVar) ? "False" : "True");
	} else if (varName == "BOUNDCONDITIONS") {
		if (smd->domain->manta_solver_res == 2) {
			if ((smd->domain->border_collisions & MOD_SMOKE_BORDER_LEFT) == 0) ss << "x";
			if ((smd->domain->border_collisions & MOD_SMOKE_BORDER_RIGHT) == 0) ss << "X";
			if ((smd->domain->border_collisions & MOD_SMOKE_BORDER_FRONT) == 0) ss << "y";
			if ((smd->domain->border_collisions & MOD_SMOKE_BORDER_BACK) == 0) ss << "Y";
		}
		if (smd->domain->manta_solver_res == 3) {
			if ((smd->domain->border_collisions & MOD_SMOKE_BORDER_LEFT) == 0) ss << "x";
			if ((smd->domain->border_collisions & MOD_SMOKE_BORDER_RIGHT) == 0) ss << "X";
			if ((smd->domain->border_collisions & MOD_SMOKE_BORDER_FRONT) == 0) ss << "y";
			if ((smd->domain->border_collisions & MOD_SMOKE_BORDER_BACK) == 0) ss << "Y";
			if ((smd->domain->border_collisions & MOD_SMOKE_BORDER_BOTTOM) == 0) ss << "z";
			if ((smd->domain->border_collisions & MOD_SMOKE_BORDER_TOP) == 0) ss << "Z";
		}
	} else if (varName == "RES")
		ss << mMaxRes;
	else if (varName == "RESX")
		ss << mResX;
	else if (varName == "RESY")
		if (is2D) {	ss << mResZ;}
		else { 		ss << mResY;}
	else if (varName == "RESZ") {
		if (is2D){	ss << 1;}
		else { 		ss << mResZ;}
	} else if (varName == "DT_FACTOR")
		ss << smd->domain->time_scale;
	else if (varName == "CFL")
		ss << smd->domain->cfl_condition;
	else if (varName == "FPS")
		ss << md->scene->r.frs_sec / md->scene->r.frs_sec_base;
	else if (varName == "VORTICITY")
		ss << smd->domain->vorticity / mConstantScaling;
	else if (varName == "NOISE_SCALE")
		ss << smd->domain->noise_scale;
	else if (varName == "MESH_SCALE")
		ss << smd->domain->mesh_scale;
	else if (varName == "PARTICLE_SCALE")
		ss << smd->domain->particle_scale;
	else if (varName == "NOISE_RESX")
		ss << mResXNoise;
	else if (varName == "NOISE_RESY") {
		if (is2D) {	ss << mResZNoise;}
		else { 		ss << mResYNoise;}
	} else if (varName == "NOISE_RESZ") {
		if (is2D) {	ss << 1;}
		else { 		ss << mResZNoise;}
	} else if (varName == "MESH_RESX")
		ss << mResXMesh;
	else if (varName == "MESH_RESY") {
		if (is2D) {	ss << mResZMesh;}
		else { 		ss << mResYMesh;}
	} else if (varName == "MESH_RESZ") {
		if (is2D) {	ss << 1;}
		else { 		ss << mResZMesh;}
	} else if (varName == "PARTICLE_RESX")
		ss << mResXParticle;
	else if (varName == "PARTICLE_RESY") {
		if (is2D) {	ss << mResZParticle;}
		else { 		ss << mResYParticle;}
	} else if (varName == "PARTICLE_RESZ") {
		if (is2D) {	ss << 1;}
		else { 		ss << mResZParticle;}
	} else if (varName == "WLT_STR")
		ss << smd->domain->strength;
	else if (varName == "NOISE_POSSCALE")
		ss << smd->domain->noise_pos_scale;
	else if (varName == "NOISE_TIMEANIM")
		ss << smd->domain->noise_time_anim;
	else if (varName == "COLOR_R")
		ss << smd->domain->active_color[0];
	else if (varName == "COLOR_G")
		ss << smd->domain->active_color[1];
	else if (varName == "COLOR_B")
		ss << smd->domain->active_color[2];
	else if (varName == "ADVECT_ORDER")
		ss << 2;
	else if (varName == "BUOYANCY_ALPHA")
		ss << smd->domain->alpha;
	else if (varName == "BUOYANCY_BETA")
		ss << smd->domain->beta;
	else if (varName == "BURNING_RATE")
		ss << smd->domain->burning_rate;
	else if (varName == "FLAME_SMOKE")
		ss << smd->domain->flame_smoke;
	else if (varName == "IGNITION_TEMP")
		ss << smd->domain->flame_ignition;
	else if (varName == "MAX_TEMP")
		ss << smd->domain->flame_max_temp;
	else if (varName == "FLAME_SMOKE_COLOR_X")
		ss << smd->domain->flame_smoke_color[0];
	else if (varName == "FLAME_SMOKE_COLOR_Y")
		ss << smd->domain->flame_smoke_color[1];
	else if (varName == "FLAME_SMOKE_COLOR_Z")
		ss << smd->domain->flame_smoke_color[2];
	else if (varName == "CURRENT_FRAME")
		ss << md->scene->r.cfra - 1;
	else if (varName == "PARTICLE_RANDOMNESS")
		ss << smd->domain->particle_randomness;
	else if (varName == "PARTICLE_NUMBER")
		ss << smd->domain->particle_number;
	else if (varName == "PARTICLE_MINIMUM")
		ss << smd->domain->particle_minimum;
	else if (varName == "PARTICLE_MAXIMUM")
		ss << smd->domain->particle_maximum;
	else if (varName == "PARTICLE_RADIUS")
		ss << smd->domain->particle_radius;
	else if (varName == "MESH_SMOOTHEN_UPPER")
		ss << smd->domain->mesh_smoothen_upper;
	else if (varName == "MESH_SMOOTHEN_LOWER")
		ss << smd->domain->mesh_smoothen_lower;
	else if (varName == "MESH_SMOOTHEN_POS")
		ss << smd->domain->mesh_smoothen_pos;
	else if (varName == "MESH_SMOOTHEN_NEG")
		ss << smd->domain->mesh_smoothen_neg;
	else if (varName == "USING_IMPROVED_MESH")
		ss << (smd->domain->mesh_generator == SM_MESH_IMPROVED ? "True" : "False");
	else if (varName == "PARTICLE_BAND_WIDTH")
		ss << smd->domain->particle_band_width;
	else if (varName == "SNDPARTICLE_DROPLET_THRESH")
		ss << smd->domain->particle_droplet_threshold;
	else if (varName == "SNDPARTICLE_DROPLET_AMOUNT")
		ss << smd->domain->particle_droplet_amount;
	else if (varName == "SNDPARTICLE_DROPLET_LIFE")
		ss << smd->domain->particle_droplet_life;
	else if (varName == "SNDPARTICLE_DROPLET_MAX")
		ss << smd->domain->particle_droplet_max;
	else if (varName == "SNDPARTICLE_BUBBLE_RISE")
		ss << smd->domain->particle_bubble_rise;
	else if (varName == "SNDPARTICLE_BUBBLE_LIFE")
		ss << smd->domain->particle_bubble_life;
	else if (varName == "SNDPARTICLE_BUBBLE_MAX")
		ss << smd->domain->particle_bubble_max;
	else if (varName == "SNDPARTICLE_FLOATER_AMOUNT")
		ss << smd->domain->particle_floater_amount;
	else if (varName == "SNDPARTICLE_FLOATER_LIFE")
		ss << smd->domain->particle_floater_life;
	else if (varName == "SNDPARTICLE_FLOATER_MAX")
		ss << smd->domain->particle_floater_max;
	else if (varName == "SNDPARTICLE_TRACER_AMOUNT")
		ss << smd->domain->particle_tracer_amount;
	else if (varName == "SNDPARTICLE_TRACER_LIFE")
		ss << smd->domain->particle_tracer_life;
	else if (varName == "SNDPARTICLE_TRACER_MAX")
		ss << smd->domain->particle_tracer_max;
	else if (varName == "LIQUID_SURFACE_TENSION")
		ss << smd->domain->surface_tension;
	else if (varName == "FLUID_VISCOSITY")
		ss << smd->domain->viscosity_base * pow(10.0f, -smd->domain->viscosity_exponent);
	else if (varName == "FLUID_DOMAIN_SIZE")
		ss << smd->domain->domain_size;
	else if (varName == "SNDPARTICLE_TYPES") {
		if (smd->domain->particle_type & MOD_SMOKE_PARTICLE_DROP) {
			ss << "PtypeSpray";
		}
		if (smd->domain->particle_type & MOD_SMOKE_PARTICLE_BUBBLE) {
			if (!ss.str().empty()) ss << "|";
			ss << "PtypeBubble";
		}
		if (smd->domain->particle_type & MOD_SMOKE_PARTICLE_FLOAT) {
			if (!ss.str().empty()) ss << "|";
			ss << "PtypeFoam";
		}
		if (smd->domain->particle_type & MOD_SMOKE_PARTICLE_TRACER) {
			if (!ss.str().empty()) ss << "|";
			ss << "PtypeTracer";
		}
		if (ss.str().empty()) ss << "0";

	} else if (varName == "USING_SNDPARTS") {
		tmpVar = (MOD_SMOKE_PARTICLE_DROP | MOD_SMOKE_PARTICLE_BUBBLE |
				  MOD_SMOKE_PARTICLE_FLOAT | MOD_SMOKE_PARTICLE_TRACER);
		ss << (((smd->domain->particle_type & tmpVar)) ? "True" : "False");
	} else if (varName == "GUIDING_ALPHA")
		ss << smd->domain->guiding_alpha;
	else if (varName == "GUIDING_BETA")
		ss << smd->domain->guiding_beta;
	else if (varName == "GRAVITY_X")
		ss << smd->domain->gravity[0];
	else if (varName == "GRAVITY_Y")
		ss << smd->domain->gravity[1];
	else if (varName == "GRAVITY_Z")
		ss << smd->domain->gravity[2];
	else if (varName == "MANTA_EXPORT_PATH") {
		char parent_dir[1024];
		BLI_split_dir_part(smd->domain->manta_filepath, parent_dir, sizeof(parent_dir));
		ss << parent_dir;
	} else if (varName == "ID")
		ss << mCurrentID;
	else if (varName == "USING_ADAPTIVETIME")
		ss << (smd->domain->flags & MOD_SMOKE_ADAPTIVE_TIME ? "True" : "False");
	else
		std::cout << "ERROR: Unknown option: " << varName << std::endl;
	return ss.str();
}

std::string FLUID::parseLine(const std::string& line, SmokeModifierData *smd)
{
	if (line.size() == 0) return "";
	std::string res = "";
	int currPos = 0, start_del = 0, end_del = -1;
	bool readingVar = false;
	const char delimiter = '$';
	while (currPos < line.size()) {
		if (line[currPos] == delimiter && !readingVar) {
			readingVar  = true;
			start_del   = currPos + 1;
			res        += line.substr(end_del + 1, currPos - end_del -1);
		}
		else if (line[currPos] == delimiter && readingVar) {
			readingVar  = false;
			end_del     = currPos;
			res        += getRealValue(line.substr(start_del, currPos - start_del), smd);
		}
		currPos ++;
	}
	res += line.substr(end_del+1, line.size()- end_del);
	return res;
}

std::string FLUID::parseScript(const std::string& setup_string, SmokeModifierData *smd)
{
	std::istringstream f(setup_string);
	std::ostringstream res;
	std::string line = "";
	while(getline(f, line)) {
		res << parseLine(line, smd) << "\n";
	}
	return res.str();
}

void FLUID::exportSmokeScript(SmokeModifierData *smd)
{
	bool highres  = smd->domain->flags & MOD_SMOKE_NOISE;
	bool heat     = smd->domain->active_fields & SM_ACTIVE_HEAT;
	bool colors   = smd->domain->active_fields & SM_ACTIVE_COLORS;
	bool fire     = smd->domain->active_fields & SM_ACTIVE_FIRE;
	bool obstacle = smd->domain->active_fields & SM_ACTIVE_OBSTACLE;
	bool guiding  = smd->domain->active_fields & SM_ACTIVE_GUIDING;
	bool invel    = smd->domain->active_fields & SM_ACTIVE_INVEL;

	std::string manta_script;

	manta_script += manta_import
		+ fluid_variables
		+ fluid_solver
		+ fluid_alloc
		+ fluid_adaptive_time_stepping
		+ smoke_alloc
		+ smoke_bounds
		+ smoke_variables;
	
	if (heat)
		manta_script += smoke_alloc_heat_low;
	if (colors)
		manta_script += smoke_alloc_colors_low;
	if (fire)
		manta_script += smoke_alloc_fire_low;
	if (obstacle)
		manta_script += fluid_alloc_obstacle_low;
	if (guiding)
		manta_script += fluid_alloc_guiding_low;
	if (invel)
		manta_script += fluid_alloc_invel_low;

	if (highres) {
		manta_script += fluid_variables_noise
			+ fluid_solver_noise
			+ fluid_adaptive_time_stepping_noise
			+ smoke_variables_noise
			+ smoke_alloc_noise
			+ smoke_bounds_noise
			+ smoke_wavelet_turbulence_noise;

		if (colors)
			manta_script += smoke_alloc_colors_high;
		if (fire)
			manta_script += smoke_alloc_fire_high;
	}
	
	manta_script += smoke_load_data;
	if (highres)
		manta_script += smoke_load_noise;
	
	manta_script += smoke_pre_step;
	if (highres)
		manta_script += smoke_pre_step_noise;
	
	manta_script += smoke_post_step;
	if (highres)
		manta_script += smoke_post_step_noise;

	manta_script += fluid_adapt_time_step;
	if (highres)
		manta_script += fluid_adapt_time_step_noise;

	manta_script += smoke_step;
	if (highres)
		manta_script += smoke_step_noise;
	
	manta_script += smoke_adaptive_step
			+ smoke_inflow_low
			+ smoke_standalone_load
			+ fluid_standalone_load
			+ fluid_standalone;
	
	// Fill in missing variables in script
	std::string final_script = FLUID::parseScript(manta_script, smd);
	
	// Write script
	std::ofstream myfile;
	myfile.open(smd->domain->manta_filepath);
	myfile << final_script;
	myfile.close();
}

static std::string getCacheFileEnding(char cache_format)
{
	if (FLUID::with_debug)
		std::cout << "FLUID::getCacheFileEnding()" << std::endl;

	switch(cache_format) {
		case MANTA_FILE_UNI:
			return ".uni";
		case MANTA_FILE_OPENVDB:
			return ".vdb";
		case MANTA_FILE_RAW:
			return ".raw";
		case MANTA_FILE_BIN_OBJECT:
			return ".bobj.gz";
		case MANTA_FILE_OBJECT:
			return ".obj";
		default:
			if (FLUID::with_debug)
				std::cout << "Error: Could not find file extension" << std::endl;
			return "";
	}
}

int FLUID::updateFlipStructures(SmokeModifierData *smd, int framenr)
{
	if (FLUID::with_debug)
		std::cout << "FLUID::updateFlipStructures()" << std::endl;

	if (!mUsingLiquid) return 0;

	std::ostringstream ss;
	char cacheDir[FILE_MAX], targetFile[FILE_MAX];
	cacheDir[0] = '\0';
	targetFile[0] = '\0';

	std::string pformat = getCacheFileEnding(smd->domain->cache_particle_format);
	BLI_path_join(cacheDir, sizeof(cacheDir), smd->domain->cache_directory, FLUID_CACHE_DIR_DATA, NULL);

	// TODO (sebbas): Use pp_xl and pVel_xl when using upres simulation?

	ss << "pp_####" << pformat;
	BLI_join_dirfile(targetFile, sizeof(targetFile), cacheDir, ss.str().c_str());
	BLI_path_frame(targetFile, framenr, 0);

	if (BLI_exists(targetFile)) {
		updateParticlesFromFile(targetFile, false);
	}

	ss.str("");
	ss << "pVel_####" << pformat;
	BLI_join_dirfile(targetFile, sizeof(targetFile), cacheDir, ss.str().c_str());
	BLI_path_frame(targetFile, framenr, 0);

	if (BLI_exists(targetFile)) {
		updateParticlesFromFile(targetFile, false);
	}
	return 1;
}

int FLUID::updateMeshStructures(SmokeModifierData *smd, int framenr)
{
	if (FLUID::with_debug)
		std::cout << "FLUID::updateMeshStructures()" << std::endl;

	if (!mUsingMesh) return 0;

	std::ostringstream ss;
	char cacheDir[FILE_MAX], targetFile[FILE_MAX];
	cacheDir[0] = '\0';
	targetFile[0] = '\0';

	std::string mformat = getCacheFileEnding(smd->domain->cache_surface_format);
	BLI_path_join(cacheDir, sizeof(cacheDir), smd->domain->cache_directory, FLUID_CACHE_DIR_MESH, NULL);

	ss << "liquid_mesh_####" << mformat;
	BLI_join_dirfile(targetFile, sizeof(targetFile), cacheDir, ss.str().c_str());
	BLI_path_frame(targetFile, framenr, 0);

	if (BLI_exists(targetFile)) {
		updateMeshFromFile(targetFile);
	}
	return 1;
}

int FLUID::updateParticleStructures(SmokeModifierData *smd, int framenr)
{
	if (FLUID::with_debug)
		std::cout << "FLUID::updateParticleStructures()" << std::endl;

	if (!mUsingDrops && !mUsingBubbles && !mUsingFloats && !mUsingTracers) return 0;

	std::ostringstream ss;
	char cacheDir[FILE_MAX], targetFile[FILE_MAX];
	cacheDir[0] = '\0';
	targetFile[0] = '\0';

	std::string pformat = getCacheFileEnding(smd->domain->cache_particle_format);
	BLI_path_join(cacheDir, sizeof(cacheDir), smd->domain->cache_directory, FLUID_CACHE_DIR_PARTICLES, NULL);

	ss << "ppSnd_####" << pformat;
	BLI_join_dirfile(targetFile, sizeof(targetFile), cacheDir, ss.str().c_str());
	BLI_path_frame(targetFile, framenr, 0);

	if (BLI_exists(targetFile)) {
		updateParticlesFromFile(targetFile, true);
	}

	ss.str("");
	ss << "pVelSnd_####" << pformat;
	BLI_join_dirfile(targetFile, sizeof(targetFile), cacheDir, ss.str().c_str());
	BLI_path_frame(targetFile, framenr, 0);

	if (BLI_exists(targetFile)) {
		updateParticlesFromFile(targetFile, true);
	}

	ss.str("");
	ss << "pLifeSnd_####" << pformat;
	BLI_join_dirfile(targetFile, sizeof(targetFile), cacheDir, ss.str().c_str());
	BLI_path_frame(targetFile, framenr, 0);

	if (BLI_exists(targetFile)) {
		updateParticlesFromFile(targetFile, true);
	}
	return 1;
}

/* Dirty hack: Needed to format paths from python code that is run via PyRun_SimpleString */
static std::string escapeSlashes(std::string const& s) {
	std::string result = "";
	for (std::string::const_iterator i = s.begin(), end = s.end(); i != end; ++i) {
		unsigned char c = *i;
		if (c == '\\')
			result += "\\\\";
		else
			result += c;
	}
	return result;
}

int FLUID::writeData(SmokeModifierData *smd, int framenr)
{
	if (with_debug)
		std::cout << "FLUID::writeData()" << std::endl;

	std::ostringstream ss;
	std::vector<std::string> pythonCommands;

	char cacheDirData[FILE_MAX];
	cacheDirData[0] = '\0';

	std::string dformat = getCacheFileEnding(smd->domain->cache_volume_format);
	std::string pformat = getCacheFileEnding(smd->domain->cache_particle_format);

	BLI_path_join(cacheDirData, sizeof(cacheDirData), smd->domain->cache_directory, FLUID_CACHE_DIR_DATA, NULL);
	BLI_path_make_safe(cacheDirData);

	ss << "fluid_save_data_" << mCurrentID << "('" << escapeSlashes(cacheDirData) << "', " << framenr << ", '" << dformat << "')";
	pythonCommands.push_back(ss.str());

	if (mUsingSmoke) {
		ss.str("");
		ss << "smoke_save_data_" << mCurrentID << "('" << escapeSlashes(cacheDirData) << "', " << framenr << ", '" << dformat << "')";
		pythonCommands.push_back(ss.str());
	}
	if (mUsingLiquid) {
		ss.str("");
		ss << "liquid_save_data_" << mCurrentID << "('" << escapeSlashes(cacheDirData) << "', " << framenr << ", '" << dformat << "')";
		pythonCommands.push_back(ss.str());
		ss.str("");
		ss << "liquid_save_flip_" << mCurrentID << "('" << escapeSlashes(cacheDirData) << "', " << framenr << ", '" << pformat << "')";
		pythonCommands.push_back(ss.str());
	}
	runPythonString(pythonCommands);
	return 1;
}

int FLUID::readData(SmokeModifierData *smd, int framenr)
{
	if (with_debug)
		std::cout << "FLUID::readData()" << std::endl;

	if (!mUsingSmoke && !mUsingLiquid) return 0;

	std::ostringstream ss;
	std::vector<std::string> pythonCommands;

	char cacheDirData[FILE_MAX];
	cacheDirData[0] = '\0';

	std::string dformat = getCacheFileEnding(smd->domain->cache_volume_format);
	std::string pformat = getCacheFileEnding(smd->domain->cache_particle_format);

	BLI_path_join(cacheDirData, sizeof(cacheDirData), smd->domain->cache_directory, FLUID_CACHE_DIR_DATA, NULL);
	BLI_path_make_safe(cacheDirData);

	if (mUsingSmoke) {
		ss << "smoke_load_data_" << mCurrentID << "('" << escapeSlashes(cacheDirData) << "', " << framenr << ", '" << dformat << "')";
		pythonCommands.push_back(ss.str());
	}
	if (mUsingLiquid) {
		ss << "liquid_load_data_" << mCurrentID << "('" << escapeSlashes(cacheDirData) << "', " << framenr << ", '" << dformat << "')";
		pythonCommands.push_back(ss.str());
		ss.str("");
		ss << "liquid_load_flip_" << mCurrentID << "('" << escapeSlashes(cacheDirData) << "', " << framenr << ", '" << pformat << "')";
		pythonCommands.push_back(ss.str());
	}
	runPythonString(pythonCommands);
	updatePointers();
	return 1;
}

int FLUID::readNoise(SmokeModifierData *smd, int framenr)
{
	if (with_debug)
		std::cout << "FLUID::readNoise()" << std::endl;

	if (!mUsingNoise) return 0;

	std::ostringstream ss;
	std::vector<std::string> pythonCommands;

	char cacheDirNoise[FILE_MAX];
	cacheDirNoise[0] = '\0';

	std::string nformat = getCacheFileEnding(smd->domain->cache_noise_format);

	BLI_path_join(cacheDirNoise, sizeof(cacheDirNoise), smd->domain->cache_directory, FLUID_CACHE_DIR_NOISE, NULL);
	BLI_path_make_safe(cacheDirNoise);

	if (mUsingSmoke && mUsingNoise) {
		ss << "smoke_load_noise_" << mCurrentID << "('" << escapeSlashes(cacheDirNoise) << "', " << framenr << ", '" << nformat  << "')";
		pythonCommands.push_back(ss.str());
	}
	runPythonString(pythonCommands);
	updatePointersHigh();
	return 1;
}

int FLUID::readMesh(SmokeModifierData *smd, int framenr)
{
	// dummmy function, use updateMeshFromFile
	if (with_debug)
		std::cout << "FLUID::readMesh() - dummmy function, use updateMeshFromFile()" << std::endl;

	if (!mUsingMesh) return 0;

	return 1;
}

int FLUID::readParticles(SmokeModifierData *smd, int framenr)
{
	if (with_debug)
		std::cout << "FLUID::readParticles()" << std::endl;

	if (!mUsingDrops && !mUsingBubbles && !mUsingFloats && !mUsingTracers) return 0;

	std::ostringstream ss;
	std::vector<std::string> pythonCommands;

	char cacheDirParticles[FILE_MAX];
	cacheDirParticles[0] = '\0';

	std::string pformat = getCacheFileEnding(smd->domain->cache_particle_format);

	BLI_path_join(cacheDirParticles, sizeof(cacheDirParticles), smd->domain->cache_directory, FLUID_CACHE_DIR_PARTICLES, NULL);
	BLI_path_make_safe(cacheDirParticles);

	if (mUsingDrops || mUsingBubbles || mUsingFloats || mUsingTracers) {
		ss << "fluid_load_particles_" << mCurrentID << "('" << escapeSlashes(cacheDirParticles) << "', " << framenr << ", '" << pformat << "')";
		pythonCommands.push_back(ss.str());
	}
	runPythonString(pythonCommands);
	updatePointers();
	return 1;
}

int FLUID::bakeData(SmokeModifierData *smd, int framenr)
{
	if (with_debug)
		std::cout << "FLUID::bakeData()" << std::endl;

	std::string tmpString, finalString;
	std::ostringstream ss;
	std::vector<std::string> pythonCommands;

	char cacheDirData[FILE_MAX];
	cacheDirData[0] = '\0';

	std::string dformat = getCacheFileEnding(smd->domain->cache_volume_format);
	std::string pformat = getCacheFileEnding(smd->domain->cache_particle_format);

	BLI_path_join(cacheDirData, sizeof(cacheDirData), smd->domain->cache_directory, FLUID_CACHE_DIR_DATA, NULL);
	BLI_path_make_safe(cacheDirData);

	ss << "bake_fluid_data_" << mCurrentID << "('" << escapeSlashes(cacheDirData) << "', " << framenr << ", '" << dformat << "', '" << pformat << "')";
	pythonCommands.push_back(ss.str());

	runPythonString(pythonCommands);
	return 1;
}

int FLUID::bakeNoise(SmokeModifierData *smd, int framenr)
{
	if (with_debug)
		std::cout << "FLUID::bakeNoise()" << std::endl;

	std::ostringstream ss;
	std::vector<std::string> pythonCommands;

	char cacheDirData[FILE_MAX], cacheDirNoise[FILE_MAX];
	cacheDirData[0] = '\0';
	cacheDirNoise[0] = '\0';

	std::string dformat = getCacheFileEnding(smd->domain->cache_volume_format);
	std::string nformat = getCacheFileEnding(smd->domain->cache_noise_format);

	BLI_path_join(cacheDirData, sizeof(cacheDirData), smd->domain->cache_directory, FLUID_CACHE_DIR_DATA, NULL);
	BLI_path_join(cacheDirNoise, sizeof(cacheDirNoise), smd->domain->cache_directory, FLUID_CACHE_DIR_NOISE, NULL);
	BLI_path_make_safe(cacheDirData);
	BLI_path_make_safe(cacheDirNoise);

	ss << "bake_noise_" << mCurrentID << "('" << escapeSlashes(cacheDirData) << "', '" << escapeSlashes(cacheDirNoise) << "', " << framenr << ", '" << dformat << "', '" << nformat << "')";
	pythonCommands.push_back(ss.str());

	runPythonString(pythonCommands);
	return 1;
}

int FLUID::bakeMesh(SmokeModifierData *smd, int framenr)
{
	if (with_debug)
		std::cout << "FLUID::bakeMesh()" << std::endl;

	std::ostringstream ss;
	std::vector<std::string> pythonCommands;

	char cacheDirData[FILE_MAX], cacheDirMesh[FILE_MAX];
	cacheDirData[0] = '\0';
	cacheDirMesh[0] = '\0';

	std::string dformat = getCacheFileEnding(smd->domain->cache_volume_format);
	std::string mformat = getCacheFileEnding(smd->domain->cache_surface_format);
	std::string pformat = getCacheFileEnding(smd->domain->cache_particle_format);

	BLI_path_join(cacheDirData, sizeof(cacheDirData), smd->domain->cache_directory, FLUID_CACHE_DIR_DATA, NULL);
	BLI_path_join(cacheDirMesh, sizeof(cacheDirMesh), smd->domain->cache_directory, FLUID_CACHE_DIR_MESH, NULL);
	BLI_path_make_safe(cacheDirData);
	BLI_path_make_safe(cacheDirMesh);

	ss << "bake_mesh_" << mCurrentID << "('" << escapeSlashes(cacheDirData) << "', '" << escapeSlashes(cacheDirMesh) << "', " << framenr << ", '" << dformat << "', '" << mformat << "', '" << pformat << "')";
	pythonCommands.push_back(ss.str());

	runPythonString(pythonCommands);
	return 1;
}

int FLUID::bakeParticles(SmokeModifierData *smd, int framenr)
{
	if (with_debug)
		std::cout << "FLUID::bakeParticles()" << std::endl;

	std::ostringstream ss;
	std::vector<std::string> pythonCommands;

	char cacheDirData[FILE_MAX], cacheDirParticles[FILE_MAX];
	cacheDirData[0] = '\0';
	cacheDirParticles[0] = '\0';

	std::string dformat = getCacheFileEnding(smd->domain->cache_volume_format);
	std::string pformat = getCacheFileEnding(smd->domain->cache_particle_format);

	BLI_path_join(cacheDirData, sizeof(cacheDirData), smd->domain->cache_directory, FLUID_CACHE_DIR_DATA, NULL);
	BLI_path_join(cacheDirParticles, sizeof(cacheDirParticles), smd->domain->cache_directory, FLUID_CACHE_DIR_PARTICLES, NULL);
	BLI_path_make_safe(cacheDirData);
	BLI_path_make_safe(cacheDirParticles);

	ss << "bake_particles_" << mCurrentID << "('" << escapeSlashes(cacheDirData) << "', '" << escapeSlashes(cacheDirParticles) << "', " << framenr << ", '" << dformat << "', '" << pformat << "')";
	pythonCommands.push_back(ss.str());

	runPythonString(pythonCommands);
	return 1;
}

void FLUID::updateVariablesLow(SmokeModifierData *smd)
{
	std::string tmpString, finalString;
	std::vector<std::string> pythonCommands;

	tmpString += fluid_variables;
	if (mUsingSmoke)
		tmpString += smoke_variables;
	if (mUsingLiquid)
		tmpString += liquid_variables;
	finalString = parseScript(tmpString, smd);
	pythonCommands.push_back(finalString);

	runPythonString(pythonCommands);
}

void FLUID::updateVariablesHigh(SmokeModifierData *smd)
{
	std::string tmpString, finalString;
	std::vector<std::string> pythonCommands;

	tmpString += fluid_variables_noise;
	if (mUsingSmoke)
		tmpString += smoke_variables_noise;
	finalString = parseScript(tmpString, smd);
	pythonCommands.push_back(finalString);

	runPythonString(pythonCommands);
}

void FLUID::exportSmokeData(SmokeModifierData *smd)
{
	bool highres = smd->domain->flags & MOD_SMOKE_NOISE;
	bool obstacle = smd->domain->active_fields & SM_ACTIVE_OBSTACLE;
	bool guiding  = smd->domain->active_fields & SM_ACTIVE_GUIDING;
	bool invel    = smd->domain->active_fields & SM_ACTIVE_INVEL;

	char parent_dir[1024];
	BLI_split_dir_part(smd->domain->manta_filepath, parent_dir, sizeof(parent_dir));

	FLUID::saveSmokeData(parent_dir);
	if (obstacle)
		FLUID::saveFluidObstacleData(parent_dir);
	if (guiding)
		FLUID::saveFluidGuidingData(parent_dir);
	if (invel)
		FLUID::saveFluidInvelData(parent_dir);
	if (highres)
		FLUID::saveSmokeDataHigh(parent_dir);
}

void FLUID::exportLiquidScript(SmokeModifierData *smd)
{
	bool highres  = smd->domain->flags & MOD_SMOKE_NOISE;
	bool obstacle = smd->domain->active_fields & SM_ACTIVE_OBSTACLE;
	bool guiding  = smd->domain->active_fields & SM_ACTIVE_GUIDING;
	bool invel    = smd->domain->active_fields & SM_ACTIVE_INVEL;
	bool drops    = smd->domain->particle_type & MOD_SMOKE_PARTICLE_DROP;
	bool bubble   = smd->domain->particle_type & MOD_SMOKE_PARTICLE_BUBBLE;
	bool floater  = smd->domain->particle_type & MOD_SMOKE_PARTICLE_FLOAT;
	bool tracer   = smd->domain->particle_type & MOD_SMOKE_PARTICLE_TRACER;

	std::string manta_script;
	
	manta_script += manta_import
		+ fluid_variables
		+ fluid_solver
		+ fluid_alloc
		+ fluid_adaptive_time_stepping
		+ liquid_alloc
		+ liquid_init_phi
		+ liquid_variables;

	if (obstacle)
		manta_script += fluid_alloc_obstacle_low;
	if (guiding)
		manta_script += fluid_alloc_guiding_low;
	if (invel)
		manta_script += fluid_alloc_invel_low;
	if (drops || bubble || floater || tracer)
		manta_script += fluid_alloc_sndparts;

	if (highres) {
		manta_script += fluid_variables_noise
			+ fluid_solver_noise
			+ fluid_adaptive_time_stepping_noise
			+ liquid_alloc_mesh;
	}

	manta_script += liquid_load_data;
	manta_script += liquid_load_flip;
//	if (highres)
//		manta_script += liquid_load_data_high;

	manta_script += liquid_pre_step;
	manta_script += liquid_post_step;

	manta_script += fluid_adapt_time_step;
	if (highres)
		manta_script += fluid_adapt_time_step_noise;

	manta_script += liquid_step;

	manta_script += liquid_adaptive_step
			+ liquid_standalone_load
			+ fluid_standalone_load
			+ fluid_standalone;

	std::string final_script = FLUID::parseScript(manta_script, smd);

	// Write script
	std::ofstream myfile;
	myfile.open(smd->domain->manta_filepath);
	myfile << final_script;
	myfile.close();
}

void FLUID::exportLiquidData(SmokeModifierData *smd)
{
	bool highres  = smd->domain->flags & MOD_SMOKE_NOISE;
	bool obstacle = smd->domain->active_fields & SM_ACTIVE_OBSTACLE;
	bool guiding  = smd->domain->active_fields & SM_ACTIVE_GUIDING;
	bool invel    = smd->domain->active_fields & SM_ACTIVE_INVEL;
	bool drops    = smd->domain->particle_type & MOD_SMOKE_PARTICLE_DROP;
	bool bubble   = smd->domain->particle_type & MOD_SMOKE_PARTICLE_BUBBLE;
	bool floater  = smd->domain->particle_type & MOD_SMOKE_PARTICLE_FLOAT;
	bool tracer   = smd->domain->particle_type & MOD_SMOKE_PARTICLE_TRACER;

	char parent_dir[1024];
	BLI_split_dir_part(smd->domain->manta_filepath, parent_dir, sizeof(parent_dir));
	
	FLUID::saveLiquidData(parent_dir);
	if (highres)
		FLUID::saveLiquidDataHigh(parent_dir);
	if (obstacle)
		FLUID::saveFluidObstacleData(parent_dir);
	if (guiding)
		FLUID::saveFluidGuidingData(parent_dir);
	if (invel)
		FLUID::saveFluidInvelData(parent_dir);
	if (drops || bubble || floater || tracer)
		FLUID::saveFluidSndPartsData(parent_dir);
}

/* Call Mantaflow python functions through this function. Use isAttribute for object attributes, e.g. s.cfl (here 's' is varname, 'cfl' functionName, and isAttribute true) */
static PyObject* callPythonFunction(std::string varName, std::string functionName, bool isAttribute=false)
{
	if ((varName == "") || (functionName == "")) {
		if (FLUID::with_debug)
			std::cout << "Missing Python variable name and/or function name -- name is: " << varName << ", function name is: " << functionName << std::endl;
		return NULL;
	}

	PyGILState_STATE gilstate = PyGILState_Ensure();
	PyObject *main, *var, *func, *returnedValue;

	// Get pyobject that holds result value
	main = PyImport_AddModule("__main__");
	var = PyObject_GetAttrString(main, varName.c_str());
	func = PyObject_GetAttrString(var, functionName.c_str());

	Py_DECREF(var);

	if (!isAttribute) {
		returnedValue = PyObject_CallObject(func, NULL);
		Py_DECREF(func);
	}

	PyGILState_Release(gilstate);
	return (!isAttribute) ? returnedValue : func;
}

static char* pyObjectToString(PyObject* inputObject)
{
	PyObject* encoded = PyUnicode_AsUTF8String(inputObject);
	char *result = PyBytes_AsString(encoded);
	Py_DECREF(encoded);
	Py_DECREF(inputObject);
	return result;
}

static double pyObjectToDouble(PyObject* inputObject)
{
	// Cannot use PyFloat_AsDouble() since its error check crashes - likely because of Real (aka float) type in Mantaflow
	return PyFloat_AS_DOUBLE(inputObject);
}

static long pyObjectToLong(PyObject* inputObject)
{
	return PyLong_AsLong(inputObject);
}

static void* stringToPointer(char* inputString)
{
	std::string str(inputString);
	std::istringstream in(str);
	void *dataPointer = NULL;
	in >> dataPointer;
	return dataPointer;
}

int FLUID::getFrame()
{
	if (with_debug)
		std::cout << "FLUID::getFrame()" << std::endl;
	
	std::string func = "frame";
	std::string id = std::to_string(mCurrentID);
	std::string solver = "s" + id;

	return pyObjectToLong(callPythonFunction(solver, func, true));
}

float FLUID::getTimestep()
{
	if (with_debug)
		std::cout << "FLUID::getTimestep()" << std::endl;

	std::string func = "timestep";
	std::string id = std::to_string(mCurrentID);
	std::string solver = "s" + id;

	return pyObjectToDouble(callPythonFunction(solver, func, true));
}

void FLUID::adaptTimestep()
{
	if (with_debug)
		std::cout << "FLUID::adaptTimestep()" << std::endl;

	std::vector<std::string> pythonCommands;
	std::ostringstream ss;

	ss << "fluid_adapt_time_step_" << mCurrentID << "()";
	pythonCommands.push_back(ss.str());

	runPythonString(pythonCommands);
}

void FLUID::updateMeshFromFile(const char* filename)
{
	std::string fname(filename);
	std::string::size_type idx;

	idx = fname.rfind('.');
	if(idx != std::string::npos) {
		std::string extension = fname.substr(idx+1);

		if (extension.compare("gz")==0)
			updateMeshDataFromBobj(filename);
		else if (extension.compare("obj")==0)
			updateMeshDataFromObj(filename);
		else
			std::cerr << "updateMeshDataFrom: invalid file extension in file: " << filename << std::endl;
	}
	else {
		std::cerr << "updateMeshDataFrom: unable to open file: " << filename << std::endl;
	}
}

void FLUID::updateMeshDataFromBobj(const char* filename)
{
	if (with_debug)
		std::cout << "FLUID::updateMeshDataFromBobj()" << std::endl;

	gzFile gzf;
	float fbuffer[3];
	int ibuffer[3];
	int numBuffer = 0;

	mMeshNodes->clear();
	mMeshTriangles->clear();

	gzf = (gzFile) BLI_gzopen(filename, "rb1"); // do some compression
	if (!gzf)
		std::cerr << "updateMeshData: unable to open file: " << filename << std::endl;
	
	// Num vertices
	gzread(gzf, &numBuffer, sizeof(int));

	if (with_debug)
		std::cout << "read mesh , num verts: " << numBuffer << " , in file: "<< filename << std::endl;

	if (numBuffer)
	{
		// Vertices
		mMeshNodes->resize(numBuffer);
		for (std::vector<Node>::iterator it = mMeshNodes->begin(); it != mMeshNodes->end(); ++it) {
			gzread(gzf, fbuffer, sizeof(float) * 3);
			it->pos[0] = fbuffer[0];
			it->pos[1] = fbuffer[1];
			it->pos[2] = fbuffer[2];
		}
	}
	
	// Num normals
	gzread(gzf, &numBuffer, sizeof(int));

	if (with_debug)
		std::cout << "read mesh , num normals : " << numBuffer << " , in file: "<< filename << std::endl;

	if (numBuffer)
	{
		// Normals
		if (!getNumVertices()) mMeshNodes->resize(numBuffer);
		for (std::vector<Node>::iterator it = mMeshNodes->begin(); it != mMeshNodes->end(); ++it) {
			gzread(gzf, fbuffer, sizeof(float) * 3);
			it->normal[0] = fbuffer[0];
			it->normal[1] = fbuffer[1];
			it->normal[2] = fbuffer[2];
		}
	}
	
	// Num triangles
	gzread(gzf, &numBuffer, sizeof(int));

	if (with_debug)
		std::cout << "read mesh , num triangles : " << numBuffer << " , in file: "<< filename << std::endl;

	if (numBuffer)
	{
		// Triangles
		mMeshTriangles->resize(numBuffer);
		FLUID::Triangle* bufferTriangle;
		for (std::vector<Triangle>::iterator it = mMeshTriangles->begin(); it != mMeshTriangles->end(); ++it) {
			gzread(gzf, ibuffer, sizeof(int) * 3);
			bufferTriangle = (FLUID::Triangle*) ibuffer;
			it->c[0] = bufferTriangle->c[0];
			it->c[1] = bufferTriangle->c[1];
			it->c[2] = bufferTriangle->c[2];
		}
	}
	gzclose(gzf);
}

void FLUID::updateMeshDataFromObj(const char* filename)
{
	std::ifstream ifs (filename);
	float fbuffer[3];
	int ibuffer[3];
	int cntVerts = 0, cntNormals = 0, cntTris = 0;

	mMeshNodes->clear();
	mMeshTriangles->clear();

	if (!ifs.good())
		std::cerr << "updateMeshDataFromObj: unable to open file: " << filename << std::endl;

	while(ifs.good() && !ifs.eof()) {
		std::string id;
		ifs >> id;

		if (id[0] == '#') {
			// comment
			getline(ifs, id);
			continue;
		}
		if (id == "vt") {
			// tex coord, ignore
		} else if (id == "vn") {
			// normals
			if (getNumVertices() != cntVerts)
				std::cerr << "updateMeshDataFromObj: invalid amount of mesh nodes" << std::endl;

			ifs >> fbuffer[0] >> fbuffer[1] >> fbuffer[2];
			FLUID::Node* node = &mMeshNodes->at(cntNormals);
			(*node).normal[0] = fbuffer[0];
			(*node).normal[1] = fbuffer[1];
			(*node).normal[2] = fbuffer[2];
			cntNormals++;
		} else if (id == "v") {
			// vertex
			ifs >> fbuffer[0] >> fbuffer[1] >> fbuffer[2];
			FLUID::Node node;
			node.pos[0] = fbuffer[0];
			node.pos[1] = fbuffer[1];
			node.pos[2] = fbuffer[2];
			mMeshNodes->push_back(node);
			cntVerts++;
		} else if (id == "g") {
			// group
			std::string group;
			ifs >> group;
		} else if (id == "f") {
			// face
			std::string face;
			for (int i=0; i<3; i++) {
				ifs >> face;
				if (face.find('/') != std::string::npos)
					face = face.substr(0, face.find('/')); // ignore other indices
				int idx = atoi(face.c_str()) - 1;
				if (idx < 0)
					std::cerr << "updateMeshDataFromObj: invalid face encountered" << std::endl;
				ibuffer[i] = idx;
			}
			FLUID::Triangle triangle;
			triangle.c[0] = ibuffer[0];
			triangle.c[1] = ibuffer[1];
			triangle.c[2] = ibuffer[2];
			mMeshTriangles->push_back(triangle);
			cntTris++;
		} else {
			// whatever, ignore
		}
		// kill rest of line
		getline(ifs, id);
	}
	ifs.close();
}

void FLUID::updateParticlesFromFile(const char* filename, bool isSecondary)
{
	if (with_debug)
		std::cout << "FLUID::updateParticleData()" << std::endl;

	gzFile gzf;
	float fbuffer[4];
	int ibuffer[4];

	gzf = (gzFile) BLI_gzopen(filename, "rb1"); // do some compression
	if (!gzf)
		std::cout << "updateParticleData: unable to open file" << std::endl;

	char ID[5] = {0,0,0,0,0};
	gzread(gzf, ID, 4);

	if (!strcmp(ID, "PB01")) {
		std::cout << "particle uni file format v01 not supported anymore" << std::endl;
		return;
	}

	// Pointer to FLIP system or to secondary particle system
	std::vector<pData>* dataPointer;
	std::vector<pVel>* velocityPointer;
	std::vector<float>* lifePointer;
	if (isSecondary) {
		dataPointer = mSndParticleData;
		velocityPointer = mSndParticleVelocity;
		lifePointer = mSndParticleLife;
	}
	else {
		dataPointer = mFlipParticleData;
		velocityPointer = mFlipParticleVelocity;
	}

	// pdata uni header
	const int STR_LEN_PDATA = 256;
	int elementType, bytesPerElement, numParticles, dimX, dimY, dimZ;
	char info[STR_LEN_PDATA]; // mantaflow build information
	unsigned long long timestamp; // creation time

	// read particle header
	gzread(gzf, &ibuffer, sizeof(int) * 4); // num particles, dimX, dimY, dimZ
	gzread(gzf, &elementType, sizeof(int));
	gzread(gzf, &bytesPerElement, sizeof(int));
	gzread(gzf, &info, sizeof(info));
	gzread(gzf, &timestamp, sizeof(unsigned long long));

	if (with_debug)
		std::cout << "read " << ibuffer[0] << " particles in file: "<< filename << std::endl;

	// Sanity checks
	const int partSysSize = sizeof(float) * 3 + sizeof(int);
	if (! (bytesPerElement == partSysSize) && (elementType == 0)){
		std::cout << "particle type doesn't match" << std::endl;
	}
	if (!ibuffer[0]) { // Any particles present?
		if (with_debug) std::cout << "no particles present yet" << std::endl;
		return;
	}

	// Reading base particle system file v2
	if (!strcmp(ID, "PB02"))
	{
		// Only set head fields when read from particle system and not from pdata files (possibly incomplete)
		numParticles = ibuffer[0];
		dimX = ibuffer[1];
		dimY = ibuffer[2];
		dimZ = ibuffer[3];

		dataPointer->resize(numParticles);
		FLUID::pData* bufferPData;
		for (std::vector<pData>::iterator it = dataPointer->begin(); it != dataPointer->end(); ++it) {
			gzread(gzf, fbuffer, sizeof(float) * 3 + sizeof(int));
			bufferPData = (FLUID::pData*) fbuffer;
			it->pos[0] = bufferPData->pos[0];
			it->pos[1] = bufferPData->pos[1];
			it->pos[2] = bufferPData->pos[2];
			it->flag = bufferPData->flag;
		}
	}
	// Reading particle data file v1 with velocities
	else if (!strcmp(ID, "PD01"))
	{
		numParticles = ibuffer[0];

		velocityPointer->resize(numParticles);
		FLUID::pVel* bufferPVel;
		for (std::vector<pVel>::iterator it = velocityPointer->begin(); it != velocityPointer->end(); ++it) {
			gzread(gzf, fbuffer, sizeof(float) * 3);
			bufferPVel = (FLUID::pVel*) fbuffer;
			it->pos[0] = bufferPVel->pos[0];
			it->pos[1] = bufferPVel->pos[1];
			it->pos[2] = bufferPVel->pos[2];
		}
	}
	// Reading secondary particle data extras
	else if (!strcmp(ID, "PD01"))
	{
		numParticles = ibuffer[0];

		lifePointer->resize(numParticles);
		float* bufferPLife;
		for (std::vector<float>::iterator it = lifePointer->begin(); it != lifePointer->end(); ++it) {
			gzread(gzf, fbuffer, sizeof(float));
			bufferPLife = (float*) fbuffer;
			*it = *bufferPLife;
		}
	}

	gzclose(gzf);
}

void FLUID::updatePointers()
{
	if (with_debug)
		std::cout << "FLUID::updatePointers()" << std::endl;

	std::string func = "getDataPointer";
	std::string funcNodes = "getNodesDataPointer";
	std::string funcTris  = "getTrisDataPointer";

	std::string id = std::to_string(mCurrentID);
	std::string solver = "s" + id;
	std::string parts  = "pp" + id;
	std::string snd    = "sp" + id;
	std::string mesh   = "sm" + id;
	std::string solver_ext = "_" + solver;
	std::string parts_ext  = "_" + parts;
	std::string snd_ext    = "_" + snd;
	std::string mesh_ext   = "_" + mesh;

	mObstacle  = (int*)   stringToPointer(pyObjectToString(callPythonFunction("flags" + solver_ext, func)));

	mVelocityX = (float*) stringToPointer(pyObjectToString(callPythonFunction("x_vel" + solver_ext, func)));
	mVelocityY = (float*) stringToPointer(pyObjectToString(callPythonFunction("y_vel" + solver_ext, func)));
	mVelocityZ = (float*) stringToPointer(pyObjectToString(callPythonFunction("z_vel" + solver_ext, func)));

	mForceX    = (float*) stringToPointer(pyObjectToString(callPythonFunction("x_force" + solver_ext, func)));
	mForceY    = (float*) stringToPointer(pyObjectToString(callPythonFunction("y_force" + solver_ext, func)));
	mForceZ    = (float*) stringToPointer(pyObjectToString(callPythonFunction("z_force" + solver_ext, func)));

	mPhiOutIn  = (float*) stringToPointer(pyObjectToString(callPythonFunction("phiOutIn" + solver_ext, func)));
	mFlowType  = (int*)   stringToPointer(pyObjectToString(callPythonFunction("flowType"   + solver_ext, func)));
	mNumFlow   = (int*)   stringToPointer(pyObjectToString(callPythonFunction("numFlow"    + solver_ext, func)));

	if (mUsingObstacle) {
		mPhiObsIn    = (float*) stringToPointer(pyObjectToString(callPythonFunction("phiObsIn" + solver_ext, func)));
		mNumObstacle = (int*)   stringToPointer(pyObjectToString(callPythonFunction("numObs"  + solver_ext, func)));

		mObVelocityX = (float*) stringToPointer(pyObjectToString(callPythonFunction("x_obvel" + solver_ext, func)));
		mObVelocityY = (float*) stringToPointer(pyObjectToString(callPythonFunction("y_obvel" + solver_ext, func)));
		mObVelocityZ = (float*) stringToPointer(pyObjectToString(callPythonFunction("z_obvel" + solver_ext, func)));
	}

	if (mUsingGuiding) {
		mPhiGuideIn = (float*) stringToPointer(pyObjectToString(callPythonFunction("phiGuideIn" + solver_ext, func)));
		mNumGuide   = (int*)   stringToPointer(pyObjectToString(callPythonFunction("numGuides"      + solver_ext, func)));

		mGuideVelocityX = (float*) stringToPointer(pyObjectToString(callPythonFunction("x_guidevel" + solver_ext, func)));
		mGuideVelocityY = (float*) stringToPointer(pyObjectToString(callPythonFunction("y_guidevel" + solver_ext, func)));
		mGuideVelocityZ = (float*) stringToPointer(pyObjectToString(callPythonFunction("z_guidevel" + solver_ext, func)));
	}

	if (mUsingInvel) {
		mInVelocityX = (float*) stringToPointer(pyObjectToString(callPythonFunction("x_invel" + solver_ext, func)));
		mInVelocityY = (float*) stringToPointer(pyObjectToString(callPythonFunction("y_invel" + solver_ext, func)));
		mInVelocityZ = (float*) stringToPointer(pyObjectToString(callPythonFunction("z_invel" + solver_ext, func)));
	}

	// Liquid
	if (mUsingLiquid) {
		mPhi   = (float*) stringToPointer(pyObjectToString(callPythonFunction("phi"   + solver_ext, func)));
		mPhiIn = (float*) stringToPointer(pyObjectToString(callPythonFunction("phiIn" + solver_ext, func)));

		mFlipParticleData     = (std::vector<pData>*) stringToPointer(pyObjectToString(callPythonFunction("pp"   + solver_ext, func)));
		mFlipParticleVelocity = (std::vector<pVel>*)  stringToPointer(pyObjectToString(callPythonFunction("pVel" + parts_ext,  func)));

		if (mUsingMesh) {
			mMeshNodes     = (std::vector<Node>*)     stringToPointer(pyObjectToString(callPythonFunction("mesh" + mesh_ext, funcNodes)));
			mMeshTriangles = (std::vector<Triangle>*) stringToPointer(pyObjectToString(callPythonFunction("mesh" + mesh_ext, funcTris)));
		}

		if (mUsingDrops || mUsingBubbles || mUsingFloats || mUsingTracers) {
			mSndParticleData     = (std::vector<pData>*) stringToPointer(pyObjectToString(callPythonFunction("ppSnd"    + snd_ext, func)));
			mSndParticleVelocity = (std::vector<pVel>*)  stringToPointer(pyObjectToString(callPythonFunction("pVelSnd"  + parts_ext,  func)));
			mSndParticleLife     = (std::vector<float>*) stringToPointer(pyObjectToString(callPythonFunction("pLifeSnd" + parts_ext,  func)));
		}
	}
	
	// Smoke
	if (mUsingSmoke) {
		mDensity        = (float*) stringToPointer(pyObjectToString(callPythonFunction("density"    + solver_ext, func)));
		mEmissionIn     = (float*) stringToPointer(pyObjectToString(callPythonFunction("emissionIn" + solver_ext, func)));
		mShadow         = (float*) stringToPointer(pyObjectToString(callPythonFunction("shadow"     + solver_ext, func)));

		if (mUsingHeat) {
			mHeat       = (float*) stringToPointer(pyObjectToString(callPythonFunction("heat"   + solver_ext,    func)));
		}
		if (mUsingFire) {
			mFlame      = (float*) stringToPointer(pyObjectToString(callPythonFunction("flame"  + solver_ext,   func)));
			mFuel       = (float*) stringToPointer(pyObjectToString(callPythonFunction("fuel"   + solver_ext,   func)));
			mReact      = (float*) stringToPointer(pyObjectToString(callPythonFunction("react"  + solver_ext,   func)));
		}
		if (mUsingColors) {
			mColorR     = (float*) stringToPointer(pyObjectToString(callPythonFunction("color_r"   + solver_ext, func)));
			mColorG     = (float*) stringToPointer(pyObjectToString(callPythonFunction("color_g"   + solver_ext, func)));
			mColorB     = (float*) stringToPointer(pyObjectToString(callPythonFunction("color_b"   + solver_ext, func)));
		}
	}
}

void FLUID::updatePointersHigh()
{
	if (with_debug)
		std::cout << "FLUID::updatePointersHigh()" << std::endl;

	std::string func = "getDataPointer";
	std::string id = std::to_string(mCurrentID);
	std::string solver = "s" + id;
	std::string solver_ext = "_" + solver;

	std::string noise = "sn" + id;
	std::string noise_ext = "_" + noise;

	// Liquid
	if (mUsingLiquid) {
		// Nothing to do here
	}
	
	// Smoke
	if (mUsingSmoke) {
		mDensityHigh    = (float*) stringToPointer(pyObjectToString(callPythonFunction("density"    + noise_ext, func)));
		mShadow         = (float*) stringToPointer(pyObjectToString(callPythonFunction("shadow"     + solver_ext, func)));
		mTextureU       = (float*) stringToPointer(pyObjectToString(callPythonFunction("texture_u"  + solver_ext,   func)));
		mTextureV       = (float*) stringToPointer(pyObjectToString(callPythonFunction("texture_v"  + solver_ext,   func)));
		mTextureW       = (float*) stringToPointer(pyObjectToString(callPythonFunction("texture_w"  + solver_ext,   func)));
		mTextureU2      = (float*) stringToPointer(pyObjectToString(callPythonFunction("texture_u2" + solver_ext,   func)));
		mTextureV2      = (float*) stringToPointer(pyObjectToString(callPythonFunction("texture_v2" + solver_ext,   func)));
		mTextureW2      = (float*) stringToPointer(pyObjectToString(callPythonFunction("texture_w2" + solver_ext,   func)));
		
		if (mUsingFire) {
			mFlameHigh  = (float*) stringToPointer(pyObjectToString(callPythonFunction("flame" + noise_ext, func)));
			mFuelHigh   = (float*) stringToPointer(pyObjectToString(callPythonFunction("fuel"  + noise_ext, func)));
			mReactHigh  = (float*) stringToPointer(pyObjectToString(callPythonFunction("react" + noise_ext, func)));
		}
		if (mUsingColors) {
			mColorRHigh = (float*) stringToPointer(pyObjectToString(callPythonFunction("color_r" + noise_ext, func)));
			mColorGHigh = (float*) stringToPointer(pyObjectToString(callPythonFunction("color_g" + noise_ext, func)));
			mColorBHigh = (float*) stringToPointer(pyObjectToString(callPythonFunction("color_b" + noise_ext, func)));
		}
	}
}

void FLUID::setFlipParticleData(float* buffer, int numParts)
{
	if (with_debug)
		std::cout << "FLUID::setFlipParticleData()" << std::endl;

	mFlipParticleData->resize(numParts);
	FLUID::pData* bufferPData = (FLUID::pData*) buffer;
	for (std::vector<pData>::iterator it = mFlipParticleData->begin(); it != mFlipParticleData->end(); ++it) {
		it->pos[0] = bufferPData->pos[0];
		it->pos[1] = bufferPData->pos[1];
		it->pos[2] = bufferPData->pos[2];
		it->flag = bufferPData->flag;
		bufferPData++;
	}
}

void FLUID::setSndParticleData(float* buffer, int numParts)
{
	if (with_debug)
		std::cout << "FLUID::setSndParticleData()" << std::endl;

	mSndParticleData->resize(numParts);
	FLUID::pData* bufferPData = (FLUID::pData*) buffer;
	for (std::vector<pData>::iterator it = mSndParticleData->begin(); it != mSndParticleData->end(); ++it) {
		it->pos[0] = bufferPData->pos[0];
		it->pos[1] = bufferPData->pos[1];
		it->pos[2] = bufferPData->pos[2];
		it->flag = bufferPData->flag;
		bufferPData++;
	}
}

void FLUID::setFlipParticleVelocity(float* buffer, int numParts)
{
	if (with_debug)
		std::cout << "FLUID::setFlipParticleVelocity()" << std::endl;

	mFlipParticleVelocity->resize(numParts);
	FLUID::pVel* bufferPVel = (FLUID::pVel*) buffer;
	for (std::vector<pVel>::iterator it = mFlipParticleVelocity->begin(); it != mFlipParticleVelocity->end(); ++it) {
		it->pos[0] = bufferPVel->pos[0];
		it->pos[1] = bufferPVel->pos[1];
		it->pos[2] = bufferPVel->pos[2];
		bufferPVel++;
	}
}

void FLUID::setSndParticleVelocity(float* buffer, int numParts)
{
	if (with_debug)
		std::cout << "FLUID::setSndParticleVelocity()" << std::endl;

	mSndParticleVelocity->resize(numParts);
	FLUID::pVel* bufferPVel = (FLUID::pVel*) buffer;
	for (std::vector<pVel>::iterator it = mSndParticleVelocity->begin(); it != mSndParticleVelocity->end(); ++it) {
		it->pos[0] = bufferPVel->pos[0];
		it->pos[1] = bufferPVel->pos[1];
		it->pos[2] = bufferPVel->pos[2];
		bufferPVel++;
	}
}

void FLUID::setSndParticleLife(float* buffer, int numParts)
{
	if (with_debug)
		std::cout << "FLUID::setSndParticleLife()" << std::endl;

	mSndParticleLife->resize(numParts);
	float* bufferPType = buffer;
	for (std::vector<float>::iterator it = mSndParticleLife->begin(); it != mSndParticleLife->end(); ++it) {
		*it = *bufferPType;
		bufferPType++;
	}
}

void FLUID::saveFluidObstacleData(char *pathname)
{
	std::string path(pathname);
	std::vector<std::string> pythonCommands;
	std::ostringstream ss;

	ss <<  "save_fluid_obstacle_data_low_" << mCurrentID << "(r'" << path << "')";
	pythonCommands.push_back(ss.str());

	runPythonString(pythonCommands);
}

void FLUID::saveFluidGuidingData(char *pathname)
{
	std::string path(pathname);
	std::vector<std::string> pythonCommands;
	std::ostringstream ss;

	ss <<  "save_fluid_guiding_data_low_" << mCurrentID << "(r'" << path << "')";
	pythonCommands.push_back(ss.str());

	runPythonString(pythonCommands);
}

void FLUID::saveFluidInvelData(char *pathname)
{
	std::string path(pathname);
	std::vector<std::string> pythonCommands;
	std::ostringstream ss;

	ss <<  "save_fluid_invel_data_low_" << mCurrentID << "(r'" << path << "')";
	pythonCommands.push_back(ss.str());

	runPythonString(pythonCommands);
}

void FLUID::saveFluidSndPartsData(char *pathname)
{
	std::string path(pathname);
	std::vector<std::string> pythonCommands;
	std::ostringstream ss;

	ss <<  "save_fluid_sndparts_data_low_" << mCurrentID << "(r'" << path << "')";
	pythonCommands.push_back(ss.str());

	runPythonString(pythonCommands);
}

void FLUID::saveSmokeData(char *pathname)
{
	std::string path(pathname);
	std::vector<std::string> pythonCommands;
	std::ostringstream ss;

	ss <<  "smoke_data_export_low_" << mCurrentID << "(r'" << path << "')";
	pythonCommands.push_back(ss.str());

	runPythonString(pythonCommands);
}

void FLUID::saveSmokeDataHigh(char *pathname)
{
	std::string path(pathname);
	std::vector<std::string> pythonCommands;
	std::ostringstream ss;

	ss << "save_smoke_data_high_" << mCurrentID << "(r'" << path << "')";
	pythonCommands.push_back(ss.str());

	runPythonString(pythonCommands);
}

void FLUID::saveLiquidData(char *pathname)
{
	std::string path(pathname);
	std::vector<std::string> pythonCommands;
	std::ostringstream ss;

	ss << "liquid_save_mesh_high_" << mCurrentID << "(\"" << escapeSlashes(path) << "\")\r\n";
	pythonCommands.push_back(ss.str());

	runPythonString(pythonCommands);
}

void FLUID::saveLiquidDataHigh(char *pathname)
{
	std::string path(pathname);
	std::vector<std::string> pythonCommands;
	std::ostringstream ss;

	ss <<  "save_liquid_data_high_" << mCurrentID << "(r'" << path << "')";
	pythonCommands.push_back(ss.str());

	runPythonString(pythonCommands);
}



