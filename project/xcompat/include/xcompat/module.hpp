#pragma once

#include <filesystem>

#include <hydra/img.hpp>
#include <hydra/nt/hnd.hpp>

namespace xcompat {
	class module : public hy::img {
		std::string name;
	};
}
