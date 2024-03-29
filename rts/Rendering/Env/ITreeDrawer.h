/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef _I_TREE_DRAWER_H_
#define _I_TREE_DRAWER_H_

#include "Rendering/GL/myGL.h"
#include "System/EventClient.h"
#include "System/float3.h"

const int TREE_SQUARE_SIZE = 64;
static const float MID_TREE_DIST_FACTOR = 1.0f;
static const float FADE_TREE_DIST_FACTOR = 1.5f;
static const float FAR_TREE_DIST_FACTOR = 2.0f;

class ITreeDrawer : public CEventClient
{
public:
	ITreeDrawer();
	virtual ~ITreeDrawer();

	static ITreeDrawer* GetTreeDrawer();

	void Draw(bool drawReflection);
	virtual void Draw(float treeDistance, bool drawReflection) = 0;
	virtual void DrawGrass() {}
	virtual void DrawShadowGrass() {}
	virtual void Update() = 0;

	virtual void ResetPos(const float3& pos) = 0;

	virtual void AddTree(int type, const float3& pos, float size) = 0;
	virtual void DeleteTree(const float3& pos) = 0;

	virtual void AddFallingTree(const float3& pos, const float3& dir, int type) {}
	virtual void AddGrass(const float3& pos) {}
	virtual void RemoveGrass(int x, int z) {}
	virtual void DrawShadowPass();

	bool WantsEvent(const std::string& eventName) {
		return
			(eventName == "RenderFeatureMoved") ||
			(eventName == "RenderFeatureDestroyed");
	}
	void RenderFeatureMoved(const CFeature* feature, const float3& oldpos, const float3& newpos);
	void RenderFeatureDestroyed(const CFeature* feature, const float3& pos);

	std::vector<GLuint> delDispLists;

	float baseTreeDistance;
	bool drawTrees;

private:
	void AddTrees();
};

extern ITreeDrawer* treeDrawer;

#endif // _I_TREE_DRAWER_H_
