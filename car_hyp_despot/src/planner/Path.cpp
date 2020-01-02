
#include "Path.h"
#include<iostream>
#include "math_utils.h"
#include <fstream>
using namespace std;



int Path::nearest(const COORD pos) const {
    auto& path = *this;
    double dmin = COORD::EuclideanDistance(pos, path[0]);
    int imin = 0;
    for(int i=0; i<path.size(); i++) {
        double d = COORD::EuclideanDistance(pos, path[i]);
        if(dmin > d) {
            dmin = d;
            imin = i;
        }
    }
    return imin;
}

double Path::mindist(COORD pos) {
    COORD mc = at(nearest(pos));
    double d = COORD::EuclideanDistance(mc, pos);
    return d;
}

int Path::forward(double i, double len) const {
    auto& path = *this;
    float step=(len / ModelParams::PATH_STEP);

    if(step-(int)step>1.0-1e-5)
    {
    	step++;
    }
    i += (int)(step);

    if(i > path.size()-1) {
        i = path.size()-1;
    }
    return i;
}

double Path::getYaw(int i) const {
    auto& path = *this;
	//TODO review this code
	
	int j = forward(i, 1.0);
	if(i==j) { i = max(0, i-3);}

	const COORD& pos = path[i];
	const COORD& forward_pos = path[j];
	MyVector vec(forward_pos.x - pos.x, forward_pos.y - pos.y);
    double a = vec.GetAngle(); // is this the yaw angle?
	return a;
}

Path Path::interpolate(double max_len) const {
    auto& path = *this;
	Path p;

	const double step = ModelParams::PATH_STEP;
	double t=0, ti=0;
	for(int i=0; i<path.size()-1; i++) {
        double d = COORD::EuclideanDistance(path[i], path[i+1]);
        // assert(d < 1001.0);
		double dx = (path[i+1].x-path[i].x) / d;
		double dy = (path[i+1].y-path[i].y) / d;
		double sx = path[i].x;
		double sy = path[i].y;
		while(t < ti+d) {
			double u = t - ti;
			double nx = sx + dx*u;
			double ny = sy + dy*u;

			if (p.size()==0 || nx != p.back().x || ny != p.back().y)
				p.push_back(COORD(nx, ny));

			t += step;

			if (p.size()* ModelParams::PATH_STEP >= max_len)
				break;
		}

		ti += d;
	}
	p.push_back(path[path.size()-1]);
	return p;
}

void Path::cutjoin(const Path& p) {
	int i = max(0, nearest(p[0])-1);
	erase(begin()+i, end());
	insert(end(), p.begin()/*+1*/, p.end());
}

double Path::getlength(int start){
    auto& path = *this;

	double path_len = 0;
	for(int i=start; i<path.size()-1; i++) {
        double d = COORD::EuclideanDistance(path[i], path[i+1]);
        path_len += d;
	}
	return path_len;
}

double Path::getCurDir(int pos_along){
    auto& path = *this;

    int end_pos = min(pos_along + 150, int(path.size())-1);

	return COORD::SlopAngle(path[pos_along], path[end_pos]);
}

double CapAngle(double x){
    x = fmod(x,2*M_PI);
    if (x < 0)
        x += 2*M_PI;
    return x;
}

COORD Path::GetCrossDir(int pos_along, bool dir){
	auto& path = *this;
	COORD& pos = path[pos_along];
	double cur_dir = getCurDir(pos_along);
	if (dir) // left, ccw
		cur_dir = cur_dir + M_PI/2;
	else // right.cw
		cur_dir = cur_dir - M_PI/2; 

	cur_dir = CapAngle(cur_dir);

	return COORD(cos(cur_dir), sin(cur_dir));
}

void Path::text(){
    auto& path = *this;

	cout << "Path: ";

    for (auto point: path){
    	cout << point.x << " " << point.y << " ";
    }
    cout << endl;
}
