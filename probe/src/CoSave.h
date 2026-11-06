#pragma once

namespace probe::cosave
{
	// Registers save/load/revert callbacks that log their order and thread, and keep
	// a small record (save counter + id) so a later load shows which save it came from.
	void Register();
}
