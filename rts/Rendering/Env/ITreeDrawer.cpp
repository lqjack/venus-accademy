/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */


#include "ITreeDrawer.h"
#include "BasicTreeDrawer.h"
#include "AdvTreeDrawer.h"
#include "Game/Camera.h"
#include "Rendering/GlobalRendering.h"
#include "Sim/Features/FeatureHandler.h"
#include "Sim/Features/Feature.h"
#include "Sim/Misc/GlobalConstants.h"
#include "System/Config/ConfigHandler.h"
#include "System/EventHandler.h"
#include "System/Exceptions.h"
#include "System/Log/ILog.h"
#include "System/myMath.h"

CONFIG(int, TreeRadius)
	.defaultValue((int) (5.5f * 256))
	.minimumValue(0);

CONFIG(bool, 3DTrees).defaultValue(true).safemodeValue(false);

ITreeDrawer* treeDrawer = NULL;


ITreeDrawer::ITreeDrawer()
	: CEventClient("[ITreeDrawer]", 314444, false), drawTrees(true)
{
	eventHandler.AddClient(this);
	baseTreeDistance = configHandler->GetInt("TreeRadius") / 256.0f;
}

ITreeDrawer::~ITreeDrawer() {
	eventHandler.RemoveClient(this);
	configHandler->Set("TreeRadius", (unsigned int) (baseTreeDistance * 256));
}



void ITreeDrawer::AddTrees()
{
	for (int fID = 0; /* no test*/; fID++) {
		const CFeature* f = featureHandler->GetFeature(fID);

		if (f) {
			if (f->def->drawType >= DRAWTYPE_TREE) {
				AddTree(f->def->drawType - 1, f->pos, 1.0f);
			}
		} else {
			break;
		}
	}
}

ITreeDrawer* ITreeDrawer::GetTreeDrawer()
{
	ITreeDrawer* td = NULL;

	try {
		if (configHandler->GetBool("3DTrees")) {
			td = new CAdvTreeDrawer();
		}
	} catch (const content_error& e) {
		if (e.what()[0] != '\0') {
			LOG_L(L_ERROR, "%s", e.what());
		}
		LOG("TreeDrawer: Fallback to BasicTreeDrawer.");
		// td can not be != NULL here
		//delete td;
	}

	if (!td) {
		td = new CBasicTreeDrawer();
	}

	td->AddTrees();

	return td;
}



void ITreeDrawer::DrawShadowPass()
{
}

void ITreeDrawer::Draw(bool drawReflection)
{
	const float maxDistance = CGlobalRendering::MAX_VIEW_RANGE / (SQUARE_SIZE * TREE_SQUARE_SIZE);
	const float treeDistance = Clamp(baseTreeDistance, 1.0f, maxDistance);
	// call the subclasses draw method
	Draw(treeDistance, drawReflection);
}

void ITreeDrawer::Update() {

	GML_STDMUTEX_LOCK(tree); // Update

	std::vector<GLuint>::iterator i;
	for (i = delDispLists.begin(); i != delDispLists.end(); ++i) {
		glDeleteLists(*i, 1);
	}
	delDispLists.clear();
}

void ITreeDrawer::RenderFeatureMoved(const CFeature* feature, const float3& oldpos, const float3& newpos) {
	if (feature->def->drawType >= DRAWTYPE_TREE) {
		DeleteTree(oldpos);
		AddTree(feature->def->drawType - 1, newpos, 1.0f);
	}
}

void ITreeDrawer::RenderFeatureDestroyed(const CFeature* feature, const float3& pos) {
	if (feature->def->drawType >= DRAWTYPE_TREE) {
		DeleteTree(pos);

		if (feature->speed.SqLength2D() > 0.25f) {
			AddFallingTree(pos, feature->speed, feature->def->drawType - 1);
		}
	}
}
