// File: stitcher.cc
// Date: Sun Sep 22 12:54:18 2013 +0800
// Author: Yuxin Wu <ppwwyyxxc@gmail.com>


#include "stitcher.hh"

#include <fstream>
#include <algorithm>

#include "feature/matcher.hh"
#include "warp.hh"
#include "transform_estimate.hh"
#include "projection.hh"
#include "match_info.hh"

#include "lib/timer.hh"
#include "lib/imgproc.hh"
#include "blender.hh"
using namespace std;
using namespace feature;

Mat32f Stitcher::build() {
	calc_feature();
	if (PANO) {
		build_bundle_warp();
		bundle.proj_method = ConnectedImages::ProjectionMethod::flat;
	} else {
	  //pairwise_match();
		assume_pano_pairwise();
		build_bundle_linear_simple();
		bundle.proj_method = ConnectedImages::ProjectionMethod::cylindrical;
	}
	print_debug("Using projection method: %d\n", bundle.proj_method);
	bundle.update_proj_range();

	return blend();
}

void Stitcher::calc_feature() {
	GuardedTimer tm("calc_feature()");
	int n = imgs.size();
	// detect feature
#pragma omp parallel for schedule(dynamic)
	REP(k, n) {
		feats[k] = feature_det->detect_feature(imgs[k]);
		print_debug("Image %d has %lu features\n", k, feats[k].size());
	}
}

void Stitcher::pairwise_match() {
	GuardedTimer tm("pairwise_match()");
	size_t n = imgs.size();

	REP(i, n) REPL(j, i + 1, n) {
		FeatureMatcher matcher(feats[i], feats[j]);
		auto match = matcher.match();
		TransformEstimation transf(match, feats[i], feats[j]);
		MatchInfo info;
		bool succ = transf.get_transform(&info);
		if (succ) {
			print_debug(
					"Connection between image %lu and %lu, ninliers=%lu, conf=%f\n",
					i, j, info.match.size(), info.confidence);
			graph[i].push_back(j);
			graph[j].push_back(i);
			pairwise_matches[i][j] = info;
			info.homo = info.homo.inverse();
			pairwise_matches[j][i] = move(info);
		}
	}
}

void Stitcher::assume_pano_pairwise() {
	GuardedTimer tm("assume_pano_pairwise()");
	int n = imgs.size();
	REP(i, n) {
		int next = (i + 1) % n;
		FeatureMatcher matcher(feats[i], feats[next]);
		auto match = matcher.match();
		TransformEstimation transf(match, feats[i], feats[next]);
		MatchInfo info;
		bool succ = transf.get_transform(&info);
		if (not succ)
			error_exit(ssprintf("Image %d and %d doesn't match.\n", i, next));
		print_debug("Match between image %d and %d, ninliers=%lu, conf=%f\n",
				i, next, info.match.size(), info.confidence);
		graph[i].push_back(next);
		graph[next].push_back(i);
		pairwise_matches[i][next] = info;
		info.homo = info.homo.inverse();
		pairwise_matches[next][i] = move(info);
	}
}

Mat32f Stitcher::blend() {
	GuardedTimer tm("blend()");
	int refw = imgs[bundle.identity_idx].width(),
			refh = imgs[bundle.identity_idx].height();
	auto homo2proj = bundle.get_homo2proj();
	auto proj2homo = bundle.get_proj2homo();

	Vec2D id_img_range = homo2proj(Vec(1, 1, 1)) - homo2proj(Vec(0, 0, 1));
	id_img_range.x *= refw, id_img_range.y *= refh;
	cout << "id_img_range" << id_img_range << endl;
	cout << "projmin:" << bundle.proj_range.min << "projmax" << bundle.proj_range.max << endl;

	Vec2D proj_min = bundle.proj_range.min;
	real_t x_len = bundle.proj_range.max.x - proj_min.x,
				 y_len = bundle.proj_range.max.y - proj_min.y,
				 x_per_pixel = id_img_range.x / refw,
				 y_per_pixel = id_img_range.y / refh,
				 target_width = x_len / x_per_pixel,
				 target_height = y_len / y_per_pixel;

	Coor size(target_width, target_height);
	cout << "Final Image Size: " << size << endl;

	auto scale_coor_to_img_coor = [&](Vec2D v) {
		v = v - proj_min;
		v.x /= x_per_pixel, v.y /= y_per_pixel;
		return Coor(v.x, v.y);
	};

	// blending
	Mat32f ret(size.y, size.x, 3);
	fill(ret, Color::NO);

	LinearBlender blender;
	for (auto& cur : bundle.component) {
		Coor top_left = scale_coor_to_img_coor(cur.range.min);
		Coor bottom_right = scale_coor_to_img_coor(cur.range.max);
		Coor diff = bottom_right - top_left;
		int w = diff.x, h = diff.y;
		Mat<Vec2D> orig_pos(h, w, 1);

		REP(i, h) REP(j, w) {
			Vec2D c((j + top_left.x) * x_per_pixel + proj_min.x, (i + top_left.y) * y_per_pixel + proj_min.y);
			Vec homo = proj2homo(Vec2D(c.x / refw, c.y / refh));
			homo.x -= 0.5 * homo.z, homo.y -= 0.5 * homo.z;	// shift center for homography
			homo.x *= refw, homo.y *= refh;
			Vec2D& p = (orig_pos.at(i, j)
					= cur.homo_inv.trans_normalize(homo)
					+ Vec2D(cur.imgptr->width()/2, cur.imgptr->height()/2));
			if (!p.isNaN() && (p.x < 0 || p.x >= cur.imgptr->width()
						|| p.y < 0 || p.y >= cur.imgptr->height()))
				p = Vec2D::NaN();
		}
		blender.add_image(top_left, orig_pos, *cur.imgptr);
	}
	blender.run(ret);
	return ret;
}

Homography Stitcher::get_transform(int f1, int f2) const {
	FeatureMatcher match(feats[f1], feats[f2]);		// this is not efficient
	auto ret = match.match();
	TransformEstimation transf(ret, feats[f1], feats[f2]);
	MatchInfo info;
	bool succ = transf.get_transform(&info);
	if (not succ)
		error_exit(ssprintf("Image %d & %d doesn't match.", f1, f2));
	return info.homo;
}

void Stitcher::straighten_simple() {
	int n = imgs.size();
	Vec2D center2 = bundle.component[n - 1].homo.trans2d(0, 0);
	Vec2D center1 = bundle.component[0].homo.trans2d(0, 0);
	float dydx = (center2.y - center1.y) / (center2.x - center1.x);
	Matrix S = Matrix::I(3);
	S.at(1, 0) = dydx;
	Matrix Sinv(3, 3);
	bool succ = S.inverse(Sinv);
	m_assert(succ);
	REP(i, n) bundle.component[i].homo = Sinv.prod(bundle.component[i].homo);
}


void Stitcher::build_bundle_linear_simple() {
	// assume pano pairwise
	bundle.component.resize(imgs.size());
	REP(i, imgs.size())
		bundle.component[i].imgptr = &imgs[i];

	int n = imgs.size(), mid = n >> 1;
	bundle.identity_idx = mid;
	bundle.component[mid].homo = Homography::I();

	auto& comp = bundle.component;

	// accumulate the transformations
	comp[mid+1].homo = pairwise_matches[mid][mid+1].homo;
	REPL(k, mid + 2, n)
		comp[k].homo = Homography(
				comp[k - 1].homo.prod(pairwise_matches[k][k-1].homo));
	comp[mid-1].homo = pairwise_matches[mid][mid-1].homo;
	REPD(k, mid - 2, 0)
		comp[k].homo = Homography(
				comp[k + 1].homo.prod(pairwise_matches[k][k+1].homo));
	// then, comp[k]: from k to identity
	bundle.calc_inverse_homo();
}

void Stitcher::build_bundle_warp() {
	bundle.component.resize(imgs.size());
	REP(i, imgs.size())
		bundle.component[i].imgptr = &imgs[i];
	calc_matrix_pano();
	bundle.calc_inverse_homo();
}


void Stitcher::calc_matrix_pano() {;
	GuardedTimer tm("calc_matrix_pano()");
	int n = imgs.size(), mid = n >> 1;
	bundle.identity_idx = mid;
	REP(i, n) bundle.component[i].homo = Homography::I();

	Timer timer;
	vector<MatchData> matches;		// matches[k]: k,k+1
	matches.resize(n-1);
	REP(k, n - 1) {
		FeatureMatcher matcher(feats[k], feats[(k + 1) % n]);
		matches[k] = matcher.match();
	}
	print_debug("match time: %lf secs\n", timer.duration());

	vector<Homography> bestmat;

	float minslope = numeric_limits<float>::max();
	float bestfactor = 1;
	if (n - mid > 1) {
		float newfactor = 1;
		// XXX: ugly
		float slope = update_h_factor(newfactor, minslope, bestfactor, bestmat, matches);
		if (bestmat.empty())
			error_exit("Failed to find hfactor");
		float centerx1 = 0, centerx2 = bestmat[0].trans2d(0, 0).x;
		float order = (centerx2 > centerx1 ? 1 : -1);
		REP(k, 3) {
			if (fabs(slope) < SLOPE_PLAIN) break;
			newfactor += (slope < 0 ? order : -order) / (5 * pow(2, k));
			slope = Stitcher::update_h_factor(newfactor, minslope, bestfactor, bestmat, matches);
		}
	}
	print_debug("Best hfactor: %lf\n", bestfactor);
	CylinderWarper warper(bestfactor);
	REP(k, n) warper.warp(imgs[k], feats[k]);

	// accumulate
	REPL(k, mid + 1, n) bundle.component[k].homo = move(bestmat[k - mid - 1]);
	REPD(i, mid - 1, 0) {
		matches[i].reverse();
		MatchInfo info;
		bool succ = TransformEstimation(
				matches[i], feats[i + 1], feats[i]).get_transform(&info);
		if (not succ)
			error_exit("The two image doesn't match. Failed");
		bundle.component[i].homo = info.homo;
	}
	REPD(i, mid - 2, 0)
		bundle.component[i].homo = Homography(
				bundle.component[i + 1].homo.prod(bundle.component[i].homo));
}

// XXX ugly hack
float Stitcher::update_h_factor(float nowfactor,
		float & minslope,
		float & bestfactor,
		vector<Homography>& mat,
		const vector<MatchData>& matches) {
	const int n = imgs.size(), mid = bundle.identity_idx;
	const int start = mid, end = n, len = end - start;

	vector<Mat32f> nowimgs;
	vector<vector<Descriptor>> nowfeats;
	REPL(k, start, end) {
		nowimgs.push_back(imgs[k].clone());
		nowfeats.push_back(feats[k]);
	}			// nowfeats[0] == feats[mid]

	CylinderWarper warper(nowfactor);
#pragma omp parallel for schedule(dynamic)
	REP(k, len)
		warper.warp(nowimgs[k], nowfeats[k]);

	vector<Homography> nowmat;		// size = len - 1
	REPL(k, 1, len) {
		MatchInfo info;
		bool succ = TransformEstimation(matches[k - 1 + mid], nowfeats[k - 1],
				nowfeats[k]).get_transform(&info);
		if (not succ)
			error_exit("The two image doesn't match. Failed");
		nowmat.emplace_back(info.homo);
	}

	REPL(k, 1, len - 1)
		nowmat[k] = nowmat[k - 1].prod(nowmat[k]);	// transform to nowimgs[0] == imgs[mid]

	Vec2D center2 = nowmat.back().trans2d(0, 0);
	const float slope = center2.y/ center2.x;
	print_debug("slope: %lf\n", slope);
	if (update_min(minslope, fabs(slope))) {
		bestfactor = nowfactor;
		mat = move(nowmat);
	}
	return slope;
}

/*
 *
 *Matrix Stitcher::shift_to_line(const vector<Vec2D>& ptr, const Vec2D& line) {
 *    int n = ptr.size();
 *    m_assert(n >= 4);
 *    Matrix left(4, 2 * n);
 *    Matrix right(1, 2 * n);
 *    REP(k, n) {
 *        auto & nowp = ptr[k];
 *        float targetx = (nowp.x - (line.y - nowp.y) / line.x) / 2;
 *        float targety = line.x * targetx + line.y;
 *        left.get(2 * k, 0) = nowp.x;
 *        left.get(2 * k, 1) = nowp.y;
 *        left.get(2 * k, 2) = left.get(2 * k, 3) = 0;
 *        right.get(2 * k, 0) = targetx;
 *
 *        left.get(2 * k + 1, 0) = left.get(2 * k + 1, 1) = 0;
 *        left.get(2 * k + 1, 2) = nowp.x;
 *        left.get(2 * k + 1, 3) = nowp.y;
 *        right.get(2 * k + 1, 0) = targety;
 *    }
 *    Matrix res(1, 4);
 *    if (!left.solve_overdetermined(res, right)) {
 *        cout << "line_fit solve failed" << endl;
 *        return move(res);
 *    }
 *    Matrix ret(3, 3);
 *    ret.get(0, 0) = res.get(0, 0);
 *    ret.get(0, 1) = res.get(1, 0);
 *    ret.get(1, 0) = res.get(2, 0);
 *    ret.get(1, 1) = res.get(3, 0);
 *    ret.get(2, 2) = 1;
 *    for (auto &i : ptr) {
 *        Vec2D project = TransformEstimation::cal_project(ret, i);
 *        cout << project << " ==?" << (line.x * project.x + line.y) << endl;
 *    }
 *    return move(ret);
 *}
 *
 */

/*
 *void Stitcher::straighten(vector<Matrix>& mat) const {
 *    int n = mat.size();
 *
 *    vector<Vec2D> centers;
 *    REP(k, n)
 *        centers.push_back(TransformEstimation::cal_project(mat[k], imgs[k]->get_center()));
 *    Vec2D kb = Stitcher::line_fit(centers);
 *    P(kb);
 *    if (fabs(kb.x) < 1e-3) return;		// already done
 *    Matrix shift = Stitcher::shift_to_line(centers, kb);
 *    P(shift);
 *    for (auto& i : mat) i = shift.prod(i);
 *}
 *
 *Vec2D Stitcher::line_fit(const std::vector<Vec2D>& pts) {
 *    int n = pts.size();
 *
 *    Matrix left(2, n);
 *    Matrix right(1, n);
 *    REP(k, n) {
 *        left.get(k, 0) = pts[k].x, left.get(k, 1) = 1;
 *        right.get(k, 0) = pts[k].y;
 *    }
 *
 *    Matrix res(0, 0);
 *    if (!left.solve_overdetermined(res, right)) {
 *        cout << "line_fit solve failed" << endl;
 *        return Vec2D(0, 0);
 *    }
 *    return Vec2D(res.get(0, 0), res.get(1, 0));
 *}
 *
 */
