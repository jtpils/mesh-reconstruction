#include <pcl/point_types.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/features/normal_3d.h>
#include <pcl/surface/gp3.h>
#include <pcl/surface/poisson.h>
#include <pcl/surface/marching_cubes_rbf.h>
#include <pcl/io/obj_io.h>
#include <pcl/common/io.h>

#ifdef TEST_BUILD
	#include <iostream>
	#include <fstream>
	#include <utility>
	#include <opencv2/core/core.hpp>
	typedef cv::Mat Mat;
	typedef struct Mesh{Mat vertices, faces; Mesh(Mat v, Mat f):vertices(v), faces(f) {};} Mesh;
#else
	#include "recon.hpp"
#endif

#define P2(x) (x)*(x)

typedef pcl::PointCloud<pcl::PointNormal> NormalCloud;

Mat estimatedNormals(Mat points);

NormalCloud::Ptr convert(const Mat points, const Mat inormals)
{
	Mat normals = estimatedNormals(points);
	NormalCloud::Ptr cloud(new NormalCloud);
	assert(points.rows == normals.rows);
	cloud->reserve(points.rows);
	for (int i=0; i<points.rows; i++) {
		pcl::PointNormal p;
		const float* point = points.ptr<float>(i);
		const float* normal = normals.ptr<float>(i);
		float confidence = cv::norm(inormals.row(i));
		for (char j=0; j<3; j++) {
			p.data[j] = point[j] / point[3];
			p.normal[j] = normal[j] * confidence;
		}
		cloud->push_back(p);
	}
	return cloud;
}

void convert(Mesh dst, std::vector<pcl::Vertices> faces)
{
	for (int i=0; i<faces.size(); i++) {
		assert(faces[i].vertices.size() == 3);
		for (char j=0; j<3; j++) {
			dst.faces.at<int32_t>(i, j) = faces[i].vertices[j];
		}
	}
}

int convert(Mesh dst, const pcl::PolygonMesh &mesh)
{
  if (mesh.cloud.data.empty())
    return -1;
  
  int nr_points  = mesh.cloud.width * mesh.cloud.height;
  unsigned point_size = static_cast<unsigned> (mesh.cloud.data.size() / nr_points);
  unsigned nr_faces = static_cast<unsigned> (mesh.polygons.size());
  
  size_t field_map[3];
	for (size_t d = 0; d < mesh.cloud.fields.size(); d++) {
		if (mesh.cloud.fields[d].datatype != sensor_msgs::PointField::FLOAT32)
			continue;
		if (mesh.cloud.fields[d].name == "x") {
			field_map[0] = mesh.cloud.fields[d].offset;
		} else if (mesh.cloud.fields[d].name == "y") {
			field_map[1] = mesh.cloud.fields[d].offset;
		} else if (mesh.cloud.fields[d].name == "z") {
			field_map[2] = mesh.cloud.fields[d].offset;
		}
	}
	
  for (int i = 0; i < nr_points; ++i) {
		float *vertex = dst.vertices.ptr<float>(i);
		for (char j=0; j<3; j++) {
			float *src = (float*) (&mesh.cloud.data[i*point_size + field_map[j]]);
			vertex[j] = *src;
    }
    vertex[3] = 1;
	}
	
	// let us ignore output point normals
	
	for (int i=0; i<nr_faces; i++) {
		assert(mesh.polygons[i].vertices.size() == 3);
		int32_t* verts = dst.faces.ptr<int32_t>(i);
		for (char j=0; j<3; j++) {
			verts[j] = mesh.polygons[i].vertices[j];
		}
	}
	
	return 0;
}

// filter out all faces larger than given size, and their vertices
void filterFinest(Mesh &mesh, float size)
{
	#ifdef TEST_BUILD
	printf("Before filtering: %i vertices, %i faces. Filtering size %g.\n", mesh.vertices.rows, mesh.faces.rows, size);
	#endif
	// mark accepted faces and vertices touched by any of these
	vector<bool> goodFaces(mesh.faces.rows, true), goodVertices(mesh.vertices.rows, false);
	for (int i=0; i<mesh.faces.rows; i++) {
		bool goodFace = true;
		int32_t* verts = mesh.faces.ptr<int32_t>(i);
		for (char j=0; j<3; j++) {
			float aw = mesh.vertices.at<float>(verts[j],3), bw = mesh.vertices.at<float>(verts[(j+1)%3],3);
			Mat d = mesh.vertices.row(verts[j])/aw - mesh.vertices.row(verts[(j+1)%3])/bw;
			float distance = cv::norm(d);
			if (distance > size) {
				goodFace = false;
				break;
			}
		}
		if (goodFace) {
			for (char j=0; j<3; j++) {
				goodVertices[verts[j]] = true;
			}
		}
		goodFaces[i] = goodFace;
	}
	// filter out the mesh structure
	vector<int32_t> reindex(mesh.vertices.rows, -1);
	int32_t writeIndex = 0;
	for (int i=0; i<mesh.vertices.rows; i++) {
		if (goodVertices[i]) {
			if (writeIndex < i)
				mesh.vertices.row(i).copyTo(mesh.vertices.row(writeIndex));
			reindex[i] = writeIndex;
			writeIndex += 1;
		}
	}
	mesh.vertices.resize(writeIndex);
	writeIndex = 0;
	for (int i=0; i<mesh.faces.rows; i++) {
		if (goodFaces[i]) {
			int32_t *dst = mesh.faces.ptr<int32_t>(writeIndex ++),
			        *src = mesh.faces.ptr<int32_t>(i);
			for (char j=0; j<3; j++) {
				assert (goodVertices[src[j]]);
				assert (reindex[src[j]] >= 0);
				dst[j] = reindex[src[j]];
			}
		}
	}
	mesh.faces.resize(writeIndex);
	#ifdef TEST_BUILD
	printf("After filtering: %i vertices, %i faces. Filtering size %g.\n", mesh.vertices.rows, mesh.faces.rows, size);
	#endif
}

// returns largest dimension of the bounding box
float boundingBoxSize(const Mat points)
// expects Cartesian points in rows, ignores 4th column if present
{
	float size = 0;
	for (char j=0; j<3; j++) {
		double min, max;
		minMaxIdx(points.col(j), &min, &max);
		if (max-min > size)
			size = max - min;
	}
	return size;
}

Mesh poissonSurface(const Mat points, const Mat normals, int degree)
{
	NormalCloud::Ptr cloud(convert(points, normals));
	
	pcl::Poisson<pcl::PointNormal> poisson;
	poisson.setConfidence(true);
	poisson.setOutputPolygons(false);
	poisson.setDegree(4);
	poisson.setIsoDivide(degree);
	poisson.setInputCloud (cloud);
	
	pcl::PolygonMesh mesh;
 	poisson.reconstruct(mesh);

	Mesh result(Mat(mesh.cloud.width * mesh.cloud.height, 4, CV_32FC1), Mat(mesh.polygons.size(), 3, CV_32SC1));
	//pcl::io::saveOBJFile("temp_poisson.obj", mesh);
	convert(result, mesh);
	
	float bbox = boundingBoxSize(result.vertices);
	float gridSize = bbox / (1<<(poisson.getDepth()-3));
	printf("boundbox %g, isodivide %i, depth %i, scale %g, gridsize %g\n", bbox, poisson.getIsoDivide(), poisson.getDepth(), poisson.getScale(), gridSize);
	filterFinest(result, 1.8*gridSize);
	
	return result;
}

Mesh poissonSurface(const Mat points, const Mat normals)
{
	return poissonSurface(points, normals, 4);
}

Mesh rbfSurface(const Mat points, const Mat normals)
{
	NormalCloud::Ptr cloud(convert(points, normals));
	
	pcl::MarchingCubesRBF<pcl::PointNormal> mc;
	mc.setInputCloud (cloud);
	
	pcl::PolygonMesh mesh;
 	mc.reconstruct(mesh);
	
	Mesh result(Mat(mesh.cloud.width * mesh.cloud.height, 4, CV_32FC1), Mat(mesh.polygons.size(), 3, CV_32SC1));
	convert(result, mesh);
	return result;
}

Mesh greedyProjection(const Mat points, const Mat normals)
{
	NormalCloud::Ptr cloud (convert(points, normals));
	
	pcl::search::KdTree<pcl::PointNormal>::Ptr tree2 (new pcl::search::KdTree<pcl::PointNormal>);
	tree2->setInputCloud (cloud);
	
	pcl::GreedyProjectionTriangulation<pcl::PointNormal> gp3;
	pcl::PolygonMesh triangles;
	
	// Set the maximum distance between connected points (maximum edge length)
	gp3.setSearchRadius (0.025);
	// Set typical values for the parameters
	gp3.setMu (2.5);
	gp3.setMaximumNearestNeighbors (100);
	gp3.setMaximumSurfaceAngle(M_PI/4); // 45 degrees
	gp3.setMinimumAngle(M_PI/18); // 10 degrees
	gp3.setMaximumAngle(2*M_PI/3); // 120 degrees
	gp3.setNormalConsistency(false);
	
	// Get result
	gp3.setInputCloud (cloud);
	gp3.setSearchMethod (tree2);
	gp3.reconstruct (triangles);
	
	// Additional vertex information
	//std::vector<int> parts = gp3.getPartIDs();
	//std::vector<int> states = gp3.getPointStates();
	
	Mesh result(points, Mat(triangles.polygons.size(), 3, CV_32SC1));
	convert(result, triangles.polygons);
	
	return result;
}

Mat estimatedNormals(Mat points)
{
	pcl::PointCloud<pcl::PointXYZ>::Ptr cloud (new pcl::PointCloud<pcl::PointXYZ>);

	cloud->reserve(points.rows);
	for (int i=0; i<points.rows; i++) {
		pcl::PointXYZ p;
		float *vertex = points.ptr<float>(i);
		for (char j=0; j<3; j++) {
			p.data[j] = vertex[j] / vertex[3];
		}
		cloud->push_back(p);
	}
	
  pcl::NormalEstimation<pcl::PointXYZ, pcl::Normal> n;
  pcl::PointCloud<pcl::Normal>::Ptr normals (new pcl::PointCloud<pcl::Normal>);
  pcl::search::KdTree<pcl::PointXYZ>::Ptr tree (new pcl::search::KdTree<pcl::PointXYZ>);
  tree->setInputCloud (cloud);
  n.setInputCloud (cloud);
  n.setSearchMethod (tree);
  n.setKSearch (20);
  n.compute (*normals);
  
  Mat result(normals->width, 3, CV_32FC1);
	for (int i=0; i<normals->width; i++) {
		float *normal = result.ptr<float>(i);
		for (char j=0; j<3; j++) {
			normal[j] = (*normals)(i,0).normal[j];
		}
	}
  return result;
}
#ifdef TEST_BUILD
int main(int argc, char**argv)
{
	//read input
	std::ifstream is("shit/bunny_1000");
	//std::ifstream is("shit/stanford_dragon_big");
	std::ofstream os("shit/bunny_poisson.obj");
  os.precision (5);
	int n;
	is >> n;
	std::cout << "Reading " << n << " points..." << std::endl;
	Mat points(0, 1, CV_32FC4);
	cv::Scalar_<float> point;
	point[3] = 1;
	for(int i=0; i < n; i ++) {
		is >> point[0] >> point[1] >> point[2];
		points.push_back(point);
	}
	is.close();
	points = points.reshape(1);
	std::cout << points.rows << " points, " << points.cols << " dimensions" << std::endl;
	for(int i=0; i < n; i ++) {
		for (int j=0; j<3; j++)
			assert (points.at<float>(i,j) <= 0 || points.at<float>(i,j) > 0);
		assert (points.at<float>(i,3) != 0);
	}
	std::cout << "Calculating normals..." << std::endl;
	Mat normals = estimatedNormals(points);
	int degree = 4;
	if (argc > 1)
		degree = atoi(argv[1]);
	std::cout << "Calculating surface at degree "<<degree <<"..." << std::endl;

	Mesh result = poissonSurface(points, normals, degree);//greedyProjection(points, normals);
	std::cout << result.vertices.rows << " vertices, " << result.faces.rows << " faces" << std::endl;
	for(int i=0; i < result.vertices.rows; i ++) {
		const float* row = result.vertices.ptr<float>(i);
		os << "v " << row[0] << ' ' << row[1] << ' ' << row[2] << std::endl;
	}
	for (int i=0; i < result.faces.rows; i++) {
		const int32_t* row = result.faces.ptr<int32_t>(i);
		os << "f " << row[0]+1 << ' ' << row[1]+1 << ' ' << row[2]+1 << std::endl;
	}
	os.close();

	return EXIT_SUCCESS;
}
#endif