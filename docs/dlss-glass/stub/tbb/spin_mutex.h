#pragma once
namespace tbb { struct spin_mutex { struct scoped_lock { scoped_lock(spin_mutex&){} scoped_lock(){} }; void lock(){} void unlock(){} }; }
