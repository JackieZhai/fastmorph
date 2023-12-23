#define PYBIND11_DETAILED_ERROR_MESSAGES

#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>
#include <pybind11/stl.h>

#include <vector>
#include <cstdlib>
#include <cmath>
#include "threadpool.h"

namespace py = pybind11;


template <typename LABEL>
py::array to_numpy(
	LABEL* output,
	const uint64_t sx, const uint64_t sy, const uint64_t sz
) {
	py::capsule capsule(output, [](void* ptr) {
		if (ptr) {
			delete[] static_cast<LABEL*>(ptr);
		}
	});

	uint64_t width = sizeof(LABEL);

	return py::array_t<LABEL>(
		{sx,sy,sz},
		{width, sx * width, sx * sy * width},
		output,
		capsule
	);
}

template <typename LABEL>
py::array dilate_helper(
	LABEL* labels, LABEL* output,
	const uint64_t sx, const uint64_t sy, const uint64_t sz,
	const bool background_only, const uint64_t threads = 1
) {

	// assume a 3x3x3 stencil with all voxels on
	const uint64_t sxy = sx * sy;

	auto fill_partial_stencil_fn = [&](
		const uint64_t xi, const uint64_t yi, const uint64_t zi, 
		std::vector<LABEL> &square
	) {
		square.clear();

		if (xi < 0 || xi >= sx) {
			return;
		}

		const uint64_t loc = xi + sx * (yi + sy * zi);

		if (labels[loc] != 0) {
			square.push_back(labels[loc]);
		}

		if (yi > 0 && labels[loc-sx] != 0) {
			square.push_back(labels[loc-sx]);
		}
		if (yi < sy - 1 && labels[loc+sx] != 0) {
			square.push_back(labels[loc+sx]);
		}
		if (zi > 0 && labels[loc-sxy] != 0) {
			square.push_back(labels[loc-sxy]);
		}
		if (zi < sz - 1 && labels[loc+sxy] != 0) {
			square.push_back(labels[loc+sxy]);
		}
		if (yi > 0 && zi > 0 && labels[loc-sx-sxy] != 0) {
			square.push_back(labels[loc-sx-sxy]);
		}
		if (yi < sy -1 && zi > 0 && labels[loc+sx-sxy] != 0) {
			square.push_back(labels[loc+sx-sxy]);
		}
		if (yi > 0 && zi < sz - 1 && labels[loc-sx+sxy] != 0) {
			square.push_back(labels[loc-sx+sxy]);
		}
		if (yi < sy - 1 && zi < sz - 1 && labels[loc+sx+sxy] != 0) {
			square.push_back(labels[loc+sx+sxy]);
		}
	};

	auto process_block = [&](
		const uint64_t xs, const uint64_t xe, 
		const uint64_t ys, const uint64_t ye, 
		const uint64_t zs, const uint64_t ze
	){
		// 3x3 sets of labels, as index advances 
		// right is leading edge, middle becomes left, 
		// left gets deleted
		std::vector<LABEL> left, middle, right;

		auto advance_stencil = [&](uint64_t x, uint64_t y, uint64_t z) {
			left = middle;
			middle = right;
			fill_partial_stencil_fn(x+2,y,z,right);
		};

		int stale_stencil = 3;


		std::vector<LABEL> neighbors;
		neighbors.reserve(27);

		for (uint64_t z = zs; z < ze; z++) {
			for (uint64_t y = ys; y < ye; y++) {
				stale_stencil = 3;
				for (uint64_t x = xs; x < xe; x++) {
					uint64_t loc = x + sx * (y + sy * z);

					if (background_only && labels[loc] != 0) {
						output[loc] = labels[loc];
						stale_stencil++;
						continue;
					}

					if (stale_stencil == 1) {
						advance_stencil(x-1,y,z);
						stale_stencil = 0;
					}
					else if (stale_stencil == 2) {
						left = right;
						fill_partial_stencil_fn(x,y,z,middle);
						fill_partial_stencil_fn(x+1,y,z,right);
						stale_stencil = 0;					
					}
					else if (stale_stencil >= 3) {
						fill_partial_stencil_fn(x-1,y,z,left);
						fill_partial_stencil_fn(x,y,z,middle);
						fill_partial_stencil_fn(x+1,y,z,right);
						stale_stencil = 0;
					}

					if (left.size() + middle.size() + right.size() == 0) {
						advance_stencil(x,y,z);
						continue;
					} 

					neighbors.clear();

					neighbors.insert(neighbors.end(), left.begin(), left.end());
					neighbors.insert(neighbors.end(), middle.begin(), middle.end());
					neighbors.insert(neighbors.end(), right.begin(), right.end());

					std::sort(neighbors.begin(), neighbors.end());

					int size = neighbors.size();

					// the middle and right will be the next
					// left and middle and will dominate the
					// right so we can skip some calculation. 
					if (size >= 19
						&& neighbors[0] == neighbors[size - 1]) {

						output[loc] = neighbors[0];
						if (x < sx - 1) {
							output[loc+1] = neighbors[0];
						}
						stale_stencil = 2;
						x++;
						continue;
					}

					LABEL mode_label = neighbors[0];
					int ct = 1;
					int max_ct = 1;
					for (int i = 1; i < size; i++) {
						if (neighbors[i] != neighbors[i-1]) {
							if (ct > max_ct) {
								mode_label = neighbors[i-1];
								max_ct = ct;
							}
							ct = 1;

							if (size - i < max_ct) {
								break;
							}
						}
						else {
							ct++;
						}
					}

					if (ct > max_ct) {
						mode_label = neighbors[size - 1];
					}

					output[loc] = mode_label;

					if (ct >= 19 && x < sx - 1) {
						output[loc+1] = mode_label;
						stale_stencil = 2;
						x++;
						continue;
					}

					stale_stencil = 1;
				}
			}
		}
	};

	const uint64_t block_size = 64;

	const uint64_t grid_x = std::max(static_cast<uint64_t>((sx + block_size/2) / block_size), static_cast<uint64_t>(1));
	const uint64_t grid_y = std::max(static_cast<uint64_t>((sy + block_size/2) / block_size), static_cast<uint64_t>(1));
	const uint64_t grid_z = std::max(static_cast<uint64_t>((sz + block_size/2) / block_size), static_cast<uint64_t>(1));

	const int real_threads = std::max(std::min(threads, grid_x * grid_y * grid_z), static_cast<uint64_t>(0));

	ThreadPool pool(real_threads);

	for (uint64_t gz = 0; gz < grid_z; gz++) {
		for (uint64_t gy = 0; gy < grid_y; gy++) {
			for (uint64_t gx = 0; gx < grid_x; gx++) {
				pool.enqueue([=]() {
					process_block(
						gx * block_size, std::min((gx+1) * block_size, sx),
						gy * block_size, std::min((gy+1) * block_size, sy),
						gz * block_size, std::min((gz+1) * block_size, sz)
					);
				});
			}
		}
	}

	pool.join();

	return to_numpy(output, sx, sy, sz);
}

// assumes fortran order
py::array dilate(
	const py::array &labels, 
	const bool background_only, 
	const int threads
) {
	int width = labels.dtype().itemsize();

	const uint64_t sx = labels.shape()[0];
	const uint64_t sy = labels.shape()[1];
	const uint64_t sz = labels.shape()[2];

	void* labels_ptr = const_cast<void*>(labels.data());
	uint8_t* output_ptr = new uint8_t[sx * sy * sz * width]();

	py::array output;

	if (width == 1) {
		output = dilate_helper(
			reinterpret_cast<uint8_t*>(labels_ptr),
			reinterpret_cast<uint8_t*>(output_ptr),
			sx, sy, sz,
			background_only, threads
		);
	}
	else if (width == 2) {
		output = dilate_helper(
			reinterpret_cast<uint16_t*>(labels_ptr),
			reinterpret_cast<uint16_t*>(output_ptr),
			sx, sy, sz,
			background_only, threads
		);
	}
	else if (width == 4) {
		output = dilate_helper(
			reinterpret_cast<uint32_t*>(labels_ptr),
			reinterpret_cast<uint32_t*>(output_ptr),
			sx, sy, sz,
			background_only, threads
		);
	}
	else if (width == 8) {
		output = dilate_helper(
			reinterpret_cast<uint64_t*>(labels_ptr),
			reinterpret_cast<uint64_t*>(output_ptr),
			sx, sy, sz,
			background_only, threads
		);
	}

	return output;
}

template <typename LABEL>
py::array erode_helper(
	LABEL* labels, LABEL* output,
	const uint64_t sx, const uint64_t sy, const uint64_t sz,
	const uint64_t threads
) {

	// assume a 3x3x3 stencil with all voxels on
	const uint64_t sxy = sx * sy;

	auto fill_partial_stencil_fn = [&](
		const uint64_t xi, const uint64_t yi, const uint64_t zi, 
		std::vector<LABEL> &square
	) {
		square.clear();

		if (xi < 0 || xi >= sx) {
			return;
		}

		const uint64_t loc = xi + sx * (yi + sy * zi);

		if (labels[loc] != 0) {
			square.push_back(labels[loc]);
		}

		if (yi > 0 && labels[loc-sx] != 0) {
			square.push_back(labels[loc-sx]);
		}
		if (yi < sy - 1 && labels[loc+sx] != 0) {
			square.push_back(labels[loc+sx]);
		}
		if (zi > 0 && labels[loc-sxy] != 0) {
			square.push_back(labels[loc-sxy]);
		}
		if (zi < sz - 1 && labels[loc+sxy] != 0) {
			square.push_back(labels[loc+sxy]);
		}
		if (yi > 0 && zi > 0 && labels[loc-sx-sxy] != 0) {
			square.push_back(labels[loc-sx-sxy]);
		}
		if (yi < sy -1 && zi > 0 && labels[loc+sx-sxy] != 0) {
			square.push_back(labels[loc+sx-sxy]);
		}
		if (yi > 0 && zi < sz - 1 && labels[loc-sx+sxy] != 0) {
			square.push_back(labels[loc-sx+sxy]);
		}
		if (yi < sy - 1 && zi < sz - 1 && labels[loc+sx+sxy] != 0) {
			square.push_back(labels[loc+sx+sxy]);
		}
	};

	auto is_pure = [](std::vector<LABEL> &square){
		if (square.size() < 9) {
			return false;
		}

		for (int i = 1; i < 9; i++) {
			if (square[i] != square[i-1]) {
				return false;
			}
		}

		return true;
	};

	auto process_block = [&](
		const uint64_t xs, const uint64_t xe, 
		const uint64_t ys, const uint64_t ye, 
		const uint64_t zs, const uint64_t ze
	){
		// 3x3 sets of labels, as index advances 
		// right is leading edge, middle becomes left, 
		// left gets deleted
		std::vector<LABEL> left, middle, right;
		bool pure_left = false;
		bool pure_middle = false;
		bool pure_right = false;

		auto advance_stencil = [&](uint64_t x, uint64_t y, uint64_t z) {
			left = middle;
			middle = right;
			pure_left = pure_middle;
			pure_middle = pure_right;
			fill_partial_stencil_fn(x+2,y,z,right);
			pure_right = is_pure(right);
		};

		int stale_stencil = 3;

		for (uint64_t z = zs; z < ze; z++) {
			for (uint64_t y = ys; y < ye; y++) {
				stale_stencil = 3;
				for (uint64_t x = xs; x < xe; x++) {
					uint64_t loc = x + sx * (y + sy * z);

					if (labels[loc] == 0) {
						stale_stencil++;
						continue;
					}

					if (stale_stencil == 1) {
						advance_stencil(x-1,y,z);
						stale_stencil = 0;
					}
					else if (stale_stencil == 2) {
						left = right;
						pure_left = pure_right;
						fill_partial_stencil_fn(x+1,y,z,right);
						pure_right = is_pure(right);
						if (!pure_right) {
							x += 2;
							stale_stencil = 3;
							continue;
						}
						fill_partial_stencil_fn(x,y,z,middle);
						pure_middle = is_pure(middle);
						stale_stencil = 0;					
					}
					else if (stale_stencil >= 3) {
						fill_partial_stencil_fn(x+1,y,z,right);
						pure_right = is_pure(right);
						if (!pure_right) {
							x += 2;
							stale_stencil = 3;
							continue;
						}
						fill_partial_stencil_fn(x,y,z,middle);
						pure_middle = is_pure(middle);
						if (!pure_middle) {
							x++;
							stale_stencil = 2;
							continue;
						}
						fill_partial_stencil_fn(x-1,y,z,left);
						pure_left = is_pure(left);
						stale_stencil = 0;
					}

					if (!pure_right) {
						x += 2;
						stale_stencil = 3;
						continue;
					}
					else if (!pure_middle) {
						x++;
						stale_stencil = 2;
						continue;
					}
					else if (pure_left) {
						if (
							labels[loc] == left[0] 
							&& labels[loc] == middle[0] 
							&& labels[loc] == right[0]
						) {
							output[loc] = labels[loc];
						}
					}

					stale_stencil = 1;
				}
			}
		}
	};

	const uint64_t block_size = 64;

	const uint64_t grid_x = std::max(static_cast<uint64_t>((sx + block_size/2) / block_size), static_cast<uint64_t>(1));
	const uint64_t grid_y = std::max(static_cast<uint64_t>((sy + block_size/2) / block_size), static_cast<uint64_t>(1));
	const uint64_t grid_z = std::max(static_cast<uint64_t>((sz + block_size/2) / block_size), static_cast<uint64_t>(1));

	const int real_threads = std::max(std::min(threads, grid_x * grid_y * grid_z), static_cast<uint64_t>(0));

	ThreadPool pool(real_threads);

	for (uint64_t gz = 0; gz < grid_z; gz++) {
		for (uint64_t gy = 0; gy < grid_y; gy++) {
			for (uint64_t gx = 0; gx < grid_x; gx++) {
				pool.enqueue([=]() {
					process_block(
						gx * block_size, std::min((gx+1) * block_size, sx),
						gy * block_size, std::min((gy+1) * block_size, sy),
						gz * block_size, std::min((gz+1) * block_size, sz)
					);
				});
			}
		}
	}

	pool.join();

	return to_numpy(output, sx, sy, sz);
}

// assumes fortran order
py::array erode(const py::array &labels, const uint64_t threads) {
	int width = labels.dtype().itemsize();

	const uint64_t sx = labels.shape()[0];
	const uint64_t sy = labels.shape()[1];
	const uint64_t sz = labels.shape()[2];

	void* labels_ptr = const_cast<void*>(labels.data());
	uint8_t* output_ptr = new uint8_t[sx * sy * sz * width]();

	py::array output;

	if (width == 1) {
		output = erode_helper(
			reinterpret_cast<uint8_t*>(labels_ptr),
			reinterpret_cast<uint8_t*>(output_ptr),
			sx, sy, sz,
			threads
		);
	}
	else if (width == 2) {
		output = erode_helper(
			reinterpret_cast<uint16_t*>(labels_ptr),
			reinterpret_cast<uint16_t*>(output_ptr),
			sx, sy, sz,
			threads
		);
	}
	else if (width == 4) {
		output = erode_helper(
			reinterpret_cast<uint32_t*>(labels_ptr),
			reinterpret_cast<uint32_t*>(output_ptr),
			sx, sy, sz,
			threads
		);
	}
	else if (width == 8) {
		output = erode_helper(
			reinterpret_cast<uint64_t*>(labels_ptr),
			reinterpret_cast<uint64_t*>(output_ptr),
			sx, sy, sz,
			threads
		);
	}

	return output;
}

PYBIND11_MODULE(fastmorphops, m) {
	m.doc() = "Accelerated fastmorph functions."; 
	m.def("dilate", &dilate, "Morphological dilation of a multilabel volume using a 3x3x3 structuring element.");
	m.def("erode", &erode, "Morphological erosion of a multilabel volume using a 3x3x3 structuring element.");
}