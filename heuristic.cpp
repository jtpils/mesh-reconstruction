// heuristic.cpp: a class encapsulating all the heuristic algorithms used

#include <opencv2/flann/flann.hpp>
#include "recon.hpp"
#include <map>

typedef cvflann::L2_Simple<float> Distance;
typedef std::pair<int, float> Neighbor;
const float focal = 0.5; // focal length of the camera P used for projection from faces

// Structure describing a camera selected by the heuristic
typedef struct {
	int index; // actual camera index in the video sequence
	float cosFromViewer, // cosine of this camera viewed from the given point
	      distance; // distance of the point to the camera, projected along the camera axis
	float viewX, viewY; // coordinates as viewed from the given point's camera
} CameraLabel;

const CameraLabel dummyLabel = {-1, 0, 0}; // camera label if selection fails

typedef std::vector< std::pair<CameraLabel, Mat> > LabelledCameras;

Heuristic::Heuristic(Configuration *iconfig)
{
	config = iconfig;
	iteration = 0;
}

// Check if the scene is detailed enough
// simply limits the number of iterations, nothing more complicated seemed appropriate
bool Heuristic::notHappy(const Mat points)
{
	iteration ++;
	return (iteration <= config->iterationCount);
}

inline float const pow2(float x)
{
	return x * x;
}

// a function that compacts two index into a single (hash table) index
inline unsigned const compact(unsigned short i, unsigned short j)
{
	return (unsigned(i) << sizeof(short)*CHAR_BIT) + unsigned(j);
}

// neighbor distance function, used in point filtering
inline float const densityFn(float dist, float radius)
{
	return (1. - dist/radius);
}

// Filter outliers and redundant points from the given point cloud
void Heuristic::filterPoints(Mat& points, Mat& normals)
{
	if (config->verbosity >= 1)
		printf("Filtering: Preparing neighbor table...\n");
	int pointCount = points.rows;
	Mat points3 = dehomogenize(points);
	
	// guess a filtering radius
	const float radius = alphaVals.back()/4.;
	
	// all distances are stored as a single 1D array, for efficiency
	// range of neighbors corresponding to i-th point is neighbors[neighborBlocks[i], ..., neighborBlocks[i+1]-1]
	std::vector<int> neighborBlocks(pointCount+1, 0);
	std::vector<Neighbor> neighbors;
	
	// == BEGIN Prepare the neighbor table ==
	neighbors.reserve(pointCount);
	{
		// FLANN search structure
		cv::flann::GenericIndex<Distance> index = cv::flann::GenericIndex<Distance>(points3, cvflann::KDTreeIndexParams());
		cvflann::SearchParams params;
		// temporary arrays to extract arguments from FLANN in correct format
		std::vector<float> distances(pointCount, 0.), point(3, 0.);
		std::vector<int> indices(pointCount, -1);
		for (int i=0; i<pointCount; i++) {
			points3.row(i).copyTo(point);
			index.radiusSearch(point, indices, distances, radius, params);
			int writeIndex = 0;
			neighborBlocks[i] = neighbors.size();
			int end;
			for (end = 0; end < indices.size() && indices[end] >= 0; end++);
			for (int j = 0; j < end; j++) {
				// to ensure symmetry, take only neighbors with a smaller index (because FLANN is randomized)
				if (indices[j] < i && distances[j] <= radius)
					neighbors.push_back(Neighbor(indices[j], densityFn(distances[j], radius)));
				indices[j] = -1;
			}
		}
	}
	neighborBlocks[pointCount] = neighbors.size();
	if (config->verbosity >= 2)
		printf(" Neighbors total: %lu, %5.1g per point.\n", neighbors.size(), ((float)neighbors.size())/pointCount);
	// == END Prepare the neighbor table ==
	
	if (config->verbosity >= 1)
		printf("Estimating local density...\n");
	
	// Calculate the local density using the power iteration scheme with clamping
	std::vector<float> density(pointCount, 1.), score(pointCount, 0.);
	double change;
	int densityIterationNo = 0;
	do {
		for (int i=0; i<pointCount; i++) {
			score[i] = 0.;
		}
		double sum = 0.;
		for (int i=0; i<pointCount; i++) {
			// add the density of each neighbor, weighted by its distance
			// and add the density of the current point to the neighbor, to ensure symmetry
			float densityTemp = 0.0;
			for (int j = neighborBlocks[i]; j < neighborBlocks[i+1]; j++) {
				densityTemp += density[neighbors[j].first] * neighbors[j].second;
				score[neighbors[j].first] += density[i] * neighbors[j].second;
				sum += (density[i] + density[neighbors[j].first]) * neighbors[j].second;
			}
			score[i] += densityTemp;
		}
		float normalizer = pointCount / sum;
		change = 0.;
		for (int i=0; i<pointCount; i++) {
			// normalize using L1 norm
			float normalizedDensity = score[i] * normalizer;
			// apply clamping
			if (normalizedDensity > 2.)
				normalizedDensity = 2.;
			// calculate the total change to stop the iteration if converged
			change += pow2(density[i] - normalizedDensity);
			density[i] = normalizedDensity;
		}
		change /= pointCount;
		densityIterationNo += 1;
	} while (change > 1e-6 && densityIterationNo < 200);
	
	// use the threshold of 0.7 (all points below this are considered outliers)
	const float densityLimit = .7;
	
	if (config->verbosity >= 2)
		printf(" Density converged in %i iterations. Limit set to: %f\n", densityIterationNo, densityLimit);
	
	// process all the points along their descending density
	std::vector<int> order(pointCount, -1);
	cv::sortIdx(density, order, cv::SORT_DESCENDING);
	int writeIndex = 0;
	
	for (int i=0; i<pointCount; i++) {
		int ord = order[i];
		// if score is too low, skip this point
		if (score[ord] < densityLimit)
			continue;
		
		// subtract density to get rid of close neighbors
		double localDensity = density[ord];
		for (int j=neighborBlocks[ord]; j<neighborBlocks[ord+1]; j++) {
			score[neighbors[j].first] -= localDensity * neighbors[j].second;
		}
		if (i > writeIndex)
			order[writeIndex] = order[i];
		writeIndex ++;
	}
	
	// filter the actual entries of the matrix
	std::sort(order.begin(), order.begin() + writeIndex);
	for (int i=0; i<writeIndex; i++) {
		if (order[i] > i) {
			// these operations may be performed directly thanks to sorting the indices three lines above
			points.row(order[i]).copyTo(points.row(i));
			normals.row(order[i]).copyTo(normals.row(i));
		}
	}
	points.resize(writeIndex);
	normals.resize(writeIndex);
}

// calculate the area of a given triangle
float faceArea(Mat points, int ia, int ib, int ic)
{
	Mat a = points.row(ia),
	    b = points.row(ib),
	    c = points.row(ic);
	a = a.colRange(0,3) / a.at<float>(3);
	b = b.colRange(0,3) / b.at<float>(3);
	c = c.colRange(0,3) / c.at<float>(3);
	Mat e = b-a,
	    f = c-b;
	return cv::norm(e.cross(f))/2;
}

// get a camera P for a given face, used in the camera selection heuristic
const Mat faceCamera(const Mesh mesh, int faceIdx, float far, float focal)
{
	const int32_t *vertIdx = mesh.faces.ptr<int32_t>(faceIdx);
	Mat a(mesh.vertices.row(vertIdx[0])),
	    b(mesh.vertices.row(vertIdx[1])),
	    c(mesh.vertices.row(vertIdx[2]));
	a = a.colRange(0,3) / a.at<float>(3);
	b = b.colRange(0,3) / b.at<float>(3);
	c = c.colRange(0,3) / c.at<float>(3);
	Mat normal((b-a).cross(c-b));
	float normalLength = cv::norm(normal);
	normal /= normalLength;

	// get a uniformly random camera center across the triangle
	float u1 = cv::randu<float>(), u2 = cv::randu<float>();
	if (u1 + u2 > 1) {
		u1 = 1-u1;
		u2 = 1-u2;
	}
	Mat center = a*u1 + b*u2 + c*(1-u1-u2);

	Mat RT;
	// ready, steady...
	float *n = normal.ptr<float>(0),
	      *ce = center.ptr<float>(0);
	float x = n[0], y = n[1], z = n[2];
	float xys = x*x + y*y, xy = sqrt(xys);
	// ...go!
	if (xy > 0) {
		// camera rotated aleng the face normal
		RT = Mat(cv::Matx44f(
			z*x/xy, z*y/xy,  xy, -z*(ce[0]*x + ce[1]*y)/xy - ce[2]*xy,
			-y/xy,    x/xy,    0,  (ce[0]*y-ce[1]*x)/xy,
			-x,        -y,       z, ce[0]*x + ce[1]*y - ce[2]*z,
			0,        0,       0,  1));
	} else {
		// no need for rotation
		float s = (z > 0) ? 1 : -1;
		RT = Mat(cv::Matx44f(
			1, 0, 0, -ce[0],
			0, s, 0, -ce[1],
			0, 0, s, -ce[2],
			0, 0, 0, 1));
	}

	// constant near value; FIXME, may cause issues and imprecision
	float near = 0.001;
	Mat K(cv::Matx44f(
		focal, 0, 0, 0,
		0, focal, 0, 0,
		0, 0, (near+far)/(far-near), 2*near*far/(near-far),
		0, 0, 1, 0));

	return K*RT;
}

// find index i such that list[i+1, ..., end] > choice
int bisect(std::vector<float> list, float choice)
{
	// no bisection, it would not make the algorithm substantially faster
	for (int i=0; i<list.size(); i++) {
		if (list[i] > choice)
			return i-1;
	}
	return list.size();
}

// find an index in a given numberedVector
// return -1 if index not in list
// else return i: list[i].first == index
int myFind(std::vector<numberedVector> list, int index)
{
	for (int i=0; i<list.size(); i++) {
		if (list[i].first == index)
			return i;
	}
	return -1;
}

// find an index in a given integer vector
// return -1 if index not in list
// else return i: list[i] == index
int myFind(std::vector<int> list, int index)
{
	for (int i=0; i<list.size(); i++) {
		if (list[i] == index)
			return i;
	}
	return -1;
}

// filter out cameras that do not display the given point on the scene surface
LabelledCameras filterCameras(Mat viewer, Mat depth, const std::vector<Mat> cameras)
{
	LabelledCameras filtered;
	// go through all cameras and check if each passes all visibility tests
	{int i=0; for (std::vector<Mat>::const_iterator camera=cameras.begin(); camera!=cameras.end(); camera++, i++) {
		CameraLabel label;
		label.index = i;
		// position of camera center projected from the face viewer matrix P
		Mat cameraFromViewer = viewer * extractCameraCenter(*camera);
		float *cfv = cameraFromViewer.ptr<float>(0);
		cameraFromViewer /= cfv[3];
		
		// check that the camera is on the correct side of the face
		cfv = cameraFromViewer.ptr<float>(0);
		if (cfv[2] > 1 || cfv[2] < -1) {
			//printf("  Failed test from viewer: %g, %g, %g\n", cfv[0], cfv[1], cfv[2]);
			continue;
		}
		label.viewX = cfv[0];
		label.viewY = cfv[1];
		
		// check that there is no obstacle between the point and the camera
		int row = (cfv[1] + 1) * depth.rows / 2,
		    col = (cfv[0] + 1) * depth.cols / 2;
		if (row < 0 || row >= depth.rows || col < 0 || col > depth.cols)
			continue;
		float obstacleDepth = depth.at<float>(row, col);
		if (obstacleDepth != backgroundDepth && obstacleDepth <= cfv[2]) {
			//printf("  Failed depth test: %g >= %g\n", cfv[2], obstacleDepth);
			continue;
		}
		
		Mat viewerCenter = extractCameraCenter(viewer);
		Mat viewerFromCamera = *camera * viewerCenter;
		float *vfc = viewerFromCamera.ptr<float>(0);
		label.distance = vfc[3] / viewerCenter.at<float>(3);

		// check that the point is in front of this camera
		if (label.distance < 0)
			continue;
		
		// check that the point is projected into image domain by this camera
		viewerFromCamera /= vfc[3];
		vfc = viewerFromCamera.ptr<float>(0);
		if (vfc[0] < -1 || vfc[0] > 1 || vfc[1] < -1 || vfc[1] > 1) {
			//printf("  Failed test from camera: %g, %g, %g\n", vfc[0], vfc[1], vfc[2]);
			continue;
		}
		
		// * camera passed all tests *
		// calculate the cosine of theta
		label.cosFromViewer = sqrt(1 / (1 + (cfv[0]*cfv[0] + cfv[1]*cfv[1])/(focal*focal)));
		filtered.push_back(std::pair<CameraLabel, Mat>(label, *camera));
	}}
	//printf(" %i cameras passed visibility tests\n", filtered.size());
	return filtered;
}

// Choose a main camera by weighted random shot
// outWeightSum is an output parameter: the sum of the unmodified weights
const CameraLabel chooseMain(std::map<unsigned, float> &weights, LabelledCameras filteredCameras, float *outWeightSum, float boostFactor)
{
	assert (filteredCameras.size() > 0);
	
	// Calculate the weights
	std::vector<float> weightSum(filteredCameras.size()+1, 0.);
	*outWeightSum = 0;
	{int i=0; for (LabelledCameras::const_iterator it = filteredCameras.begin(); it != filteredCameras.end(); it++, i++) {
		CameraLabel label = it->first;
		float weight = label.cosFromViewer/pow2(label.distance);
		// outWeightSum uses unmodified weights
		*outWeightSum += weight; 
		
		// if this main camera was selected earlier, boost its weight
		if (weights.count(compact(label.index, label.index)))
			weight += weight * boostFactor * filteredCameras.size();
		weightSum[i+1] = weightSum[i] + weight;
	}}
	
	// take the random shot
	float choice = cv::randu<float>() * weightSum.back();
	int index = bisect(weightSum, choice);
	// printf("           I shot at %g from %g and thus decided for main camera %i (at position %i), weight %g\n", choice, weightSum.back(), filteredCameras[index].first.index, index, weightSum[index+1] - weightSum[index]);
	return filteredCameras[index].first;
}

// Choose a side camera by weighted random shot
const CameraLabel chooseSide(std::map<unsigned, float> &weights, CameraLabel mainCamera, float threshold, float boostFactor, LabelledCameras filteredCameras)
{
	assert (filteredCameras.size() > 1); // mainCamera is surely in filteredCameras and we cannot pick it
	
	// Calculate the weights
	std::vector<float> weightSum(filteredCameras.size(), 0.);
	std::vector<CameraLabel> labels;
	float actualWeightSum = 0;
	int i=0;
	for (LabelledCameras::const_iterator it = filteredCameras.begin(); it != filteredCameras.end(); it++) {
		CameraLabel label = it->first;
		if (label.index == mainCamera.index)
			continue;
		// express the amount of parallax somehow
		float parallaxSqr = (pow2(label.viewX - mainCamera.viewX) + pow2(label.viewY - mainCamera.viewY)) / focal;
		float weight = label.cosFromViewer * parallaxSqr / pow2(label.distance);
		actualWeightSum += weight; // sum up the unmodified weights
		
		// if this pair of cameras was chosen earlier, boost its weight
		unsigned compactIndex = compact(mainCamera.index, label.index);
		if (weights.count(compactIndex) && weights[compactIndex] >= 1)
			weight += weight * boostFactor * filteredCameras.size();
		weightSum[i+1] = weightSum[i] + weight;
		labels.push_back(it->first);
		i++;
	}
	
	// Take the random shot
	float choice = cv::randu<float>() * weightSum.back();
	int index = bisect(weightSum, choice);
	assert(index >= 0 && index < i);
	
	unsigned compactIndex = compact(mainCamera.index, labels[index].index);
	if (weights[compactIndex] >= 1) {
		// If this pair has been selected before, do not return it
		//printf("  SKIPPED: I shot at %g from %g and thus decided for side camera %i (at position %i of %i) already picked (%g)\n", choice, weightSum.back(), labels[index].index, index, i, weights[compactIndex]);
		return dummyLabel;
	}
	
	weights[compact(mainCamera.index, mainCamera.index)] = 1; // just to make a mark
	
	// increase the summed value to that camera pair
	float addWeight = (weightSum[index+1] - weightSum[index]) / (threshold * actualWeightSum);
	weights[compactIndex] += addWeight;
	float curWeight = weights[compactIndex];
	if (curWeight >= 1) {
		// if the sum passed the threshold, return this pair
		float parallax = sqrt(pow2(labels[index].viewX - mainCamera.viewX) + pow2(labels[index].viewY - mainCamera.viewY)) / focal;
		//printf("  PASSED:  I shot at %g from %g and thus decided for side camera %i (at position %i of %i), weight %g * %g, parallax %g\n", choice, weightSum.back(), labels[index].index, index, i, curWeight, threshold * weightSum.back(), parallax);
		return labels[index];
	} else {
		//printf("  FAILED:  I shot at %g from %g and thus decided for side camera %i (at position %i of %i), weight %g * %g\n", choice, weightSum.back(), labels[index].index, index, i, curWeight, threshold * weightSum.back());
		return dummyLabel;
	}
}

// Choose all camera bundles (1 x main, n x side) for an update iteration
int Heuristic::chooseCameras(const Mesh mesh, const std::vector<Mat> cameras)
{
	chosenCameras.clear();
	int cameraCount = 0;
	std::vector<float> areaSum(mesh.faces.rows+1, 0.);
	for (int i=0; i<mesh.faces.rows; i++) {
		const int32_t *vertIdx = mesh.faces.ptr<int32_t>(i);
		areaSum[i+1] = areaSum[i] + faceArea(mesh.vertices, vertIdx[0], vertIdx[1], vertIdx[2]);
	}
	float totalArea = areaSum.back(),
	      average = totalArea / mesh.faces.rows;
	
	float samplingResolution = sqrt(cameras.size())*config->width*config->height/(totalArea * config->cameraThreshold); // units: pixels per scene-space area
	std::vector<bool> used(false, mesh.faces.rows);
	cv::RNG random = cv::theRNG();
	Render *render = spawnRender(*this);
	render->loadMesh(mesh);
	std::vector<int> empty;
	int shotCount = 200;
	// table indexed by calling compact(i,j) on two indices
	std::map<unsigned, float> weights; 
	for (int i = 0; i < shotCount; i ++) {
		// select a face by weighted randomness
		float choice = cv::randu<float>() * totalArea;
		int chosenIdx = bisect(areaSum, choice);
		
		// render a view of the scene from that face
		float far = 10; // fixme, may fail. Should be calculated from the scene geometry
		Mat viewer = faceCamera(mesh, chosenIdx, far, focal);
		Mat depth = render->depth(viewer);
		
		// filter out cameras that do not display this point correctly
		LabelledCameras filteredCameras = filterCameras(viewer, depth, cameras);
		if (filteredCameras.size() >= 2) {
			// try to pick a (main, side) camera pair
			float mainWeightSum;
			CameraLabel mainCamera = chooseMain(weights, filteredCameras, &mainWeightSum, config->cameraThreshold);
			CameraLabel sideCamera = chooseSide(weights, mainCamera, shotCount * mainWeightSum/samplingResolution, config->cameraThreshold/10, filteredCameras);
			if (sideCamera.index == dummyLabel.index) {
				// no new pair picked (or none at all)
				continue;
			}
			
			cameraCount += 1;
			// write the pair into the resulting table
			int positionMain = myFind(chosenCameras, mainCamera.index);
			if (positionMain == -1) {
				chosenCameras.push_back(numberedVector(mainCamera.index, std::vector<int>(1, sideCamera.index)));
			} else if (myFind(chosenCameras[positionMain].second, sideCamera.index) == -1) {
				chosenCameras[positionMain].second.push_back(sideCamera.index);
			}
		} else {
			// no camera pair available for this point on the scene surface
		}
	}
	delete render;
	
	// make the list a bit nicer
	std::sort(chosenCameras.begin(), chosenCameras.end());
	return cameraCount;
}

// initialize wanna-be-iterator and return frame number for first main camera
int Heuristic::beginMain()
{
	if (chosenCameras.size() == 0)
		return Heuristic::sentinel;
	else
		return chosenCameras[mainIdx = 0].first;
}

// return frame number for next main camera
int Heuristic::nextMain()
{ 
	if (++mainIdx < chosenCameras.size())
		return chosenCameras[mainIdx].first;
	else
		return Heuristic::sentinel;
}

// initialize and return frame number for first side camera
int Heuristic::beginSide(int imain)
{ 
	if (imain != chosenCameras[mainIdx].first || chosenCameras[mainIdx].second.size() == 0)
		return Heuristic::sentinel;
	else
		return chosenCameras[mainIdx].second[sideIdx = 0];
}

// return frame number for next side camera
int Heuristic::nextSide(int imain)
{ 
	if (imain != chosenCameras[mainIdx].first || ++sideIdx >= chosenCameras[mainIdx].second.size())
		return Heuristic::sentinel;
	else
		return chosenCameras[mainIdx].second[sideIdx];
}

// Polygonize the supplied point cloud using an appropriate method
Mesh Heuristic::tessellate(const Mat points, const Mat normals)
{
	if (iteration <= 1) {
		if (config->inMeshFile) {
			Mesh result = readMesh(config->inMeshFile);
			// TODO: estimate some alpha value from the geometry
			alphaVals.push_back(1);
			return result;
			assert(false);
		} else {
			float alpha;
			Mat faces = alphaShapeFaces(points, &alpha);
			alphaVals.push_back(alpha);
			return Mesh(points, faces);
		}
	} else {
		Mesh result = poissonSurface(points, normals);
		alphaVals.push_back(alphaVals.back() / 2);
		return result;
	}
}

// extract frame render size from the configuration (for reprojection)
cv::Size Heuristic::renderSize()
{
	return cv::Size(config->width, config->height);
}
