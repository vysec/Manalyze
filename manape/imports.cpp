/*
    This file is part of Manalyze.

    Manalyze is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Manalyze is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Manalyze.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "manape/pe.h"

namespace mana {

bool PE::_parse_imports()
{
	if (!_ioh || _file_handle == nullptr) { // Image Optional Header wasn't parsed successfully.
		return false;
	}
	if (!_reach_directory(IMAGE_DIRECTORY_ENTRY_IMPORT))	{ // No imports
		return true;
	}

	while (true) // We stop at the first NULL IMAGE_IMPORT_DESCRIPTOR.
	{
		pimage_import_descriptor iid(new image_import_descriptor);
		memset(iid.get(), 0, 5*sizeof(boost::uint32_t)); // Don't overwrite the last member (a string)

		if (20 != fread(iid.get(), 1, 20, _file_handle.get()))
		{
			PRINT_ERROR << "Could not read the IMAGE_IMPORT_DESCRIPTOR." << std::endl;
			return true; // Don't give up on the rest of the parsing.
		}

		// Exit condition
		if (iid->OriginalFirstThunk == 0 && iid->FirstThunk == 0) {
			break;
		}

		// Non-standard parsing. The Name RVA is translated to an actual string here.
		auto offset = _rva_to_offset(iid->Name);
		if (!offset) { // Try to use the RVA as a direct address if the imports are outside of a section.
			offset = iid->Name;
		}
		if (!utils::read_string_at_offset(_file_handle.get(), offset, iid->NameStr))
		{
			// It seems that the Windows loader doesn't give up if such a thing happens.
			if (_imports.size() > 0)
			{
				PRINT_WARNING << "Could not read an import's name." << std::endl;
				break; // Try to continue the parsing with the available imports.
			}

			PRINT_ERROR << "Could not read an import's name." << std::endl;
			return true;
		}

		pimage_library_descriptor library = boost::make_shared<image_library_descriptor>(iid, std::vector<pimport_lookup_table>());
		_imports.push_back(library);
	}

	// Parse the IMPORT_LOOKUP_TABLE for each imported library
	for (auto it = _imports.begin() ; it != _imports.end() ; ++it)
	{
		int ilt_offset;
		if ((*it)->first->OriginalFirstThunk != 0) {
			ilt_offset = _rva_to_offset((*it)->first->OriginalFirstThunk);
		}
		else { // Some packed executables use FirstThunk and set OriginalFirstThunk to 0.
			ilt_offset = _rva_to_offset((*it)->first->FirstThunk);
		}
		if (!ilt_offset || fseek(_file_handle.get(), ilt_offset, SEEK_SET))
		{
			PRINT_ERROR << "Could not reach an IMPORT_LOOKUP_TABLE." << std::endl;
			return true;
		}

		while (true) // We stop at the first NULL IMPORT_LOOKUP_TABLE
		{
			pimport_lookup_table import = boost::make_shared<import_lookup_table>();
			import->AddressOfData = 0;
			import->Hint = 0;

			// The field has a size of 8 for x64 PEs
			int size_to_read = (_ioh->Magic == nt::IMAGE_OPTIONAL_HEADER_MAGIC.at("PE32+") ? 8 : 4);
			if (size_to_read != fread(&(import->AddressOfData), 1, size_to_read, _file_handle.get()))
			{
				PRINT_ERROR << "Could not read the IMPORT_LOOKUP_TABLE." << std::endl;
				return true;
			}

			// Exit condition
			if (import->AddressOfData == 0) {
				break;
			}

			// Read the HINT/NAME TABLE if applicable. Check the most significant byte of AddressOfData to
			// see if the import is by name or ordinal. For PE32+, AddressOfData is a uint64.
			boost::uint64_t mask = (size_to_read == 8 ? 0x8000000000000000 : 0x80000000);
			if (!(import->AddressOfData & mask))
			{
				// Import by name. Read the HINT/NAME table. For both PE32 and PE32+, its RVA is stored
				// in bits 30-0 of AddressOfData.
				unsigned int table_offset = _rva_to_offset(import->AddressOfData & 0x7FFFFFFF);
				if (table_offset == 0)
				{
					PRINT_ERROR << "Could not reach the HINT/NAME table." << std::endl;
					return true;
				}

				unsigned int saved_offset = ftell(_file_handle.get());
				if (saved_offset == -1 || fseek(_file_handle.get(), table_offset, SEEK_SET) || 2 != fread(&(import->Hint), 1, 2, _file_handle.get()))
				{
					PRINT_ERROR << "Could not read a HINT/NAME hint." << std::endl;
					return true;
				}
				import->Name = utils::read_ascii_string(_file_handle.get());

				//TODO: Demangle the import name

				// Go back to the import lookup table.
				if (fseek(_file_handle.get(), saved_offset, SEEK_SET)) {
					return true;
				}
			}

			(*it)->second.push_back(import);
		}
	}

	return true;
}

// ----------------------------------------------------------------------------

const_shared_strings PE::get_imported_dlls() const
{
	auto destination = boost::make_shared<std::vector<std::string> >();
	if (!_initialized) {
		return destination;
	}

	for (std::vector<pimage_library_descriptor>::const_iterator it = _imports.begin() ; it != _imports.end() ; ++it) {
		destination->push_back((*it)->first->NameStr);
	}
	return destination;
}

// ----------------------------------------------------------------------------

const_shared_strings PE::get_imported_functions(const std::string& dll) const
{
	auto destination = boost::make_shared<std::vector<std::string> >();
	if (!_initialized) {
		return destination;
	}

	pimage_library_descriptor ild = pimage_library_descriptor();

	// We don't want to use PE::_find_imported_dlls: no regexp matching is necessary, since we only look for a simple exact name here.
	for (std::vector<pimage_library_descriptor>::const_iterator it = _imports.begin() ; it != _imports.end() ; ++it)
	{
		if ((*it)->first->NameStr == dll)
		{
			ild = *it;
			break;
		}
	}

	if (ild != nullptr)
	{
		for (std::vector<pimport_lookup_table>::const_iterator it = ild->second.begin() ; it != ild->second.end() ; ++it)
		{
			if ((*it)->Name != "") {
				destination->push_back((*it)->Name);
			}
			else
			{
				std::stringstream ss;
				ss << "#" << ((*it)->AddressOfData & 0x7FFF);
				destination->push_back(ss.str());
			}
		}
	}

	return destination;
}

// ----------------------------------------------------------------------------

std::vector<pimage_library_descriptor> PE::_find_imported_dlls(const std::string& name_regexp,
															   bool  case_sensitivity) const
{
	std::vector<pimage_library_descriptor> destination;
	if (!_initialized) {
		return destination;
	}

	boost::regex e;
	if (case_sensitivity) {
		e = boost::regex(name_regexp);
	}
	else {
		e = boost::regex(name_regexp, boost::regex::icase);
	}

	for (std::vector<pimage_library_descriptor>::const_iterator it = _imports.begin() ; it != _imports.end() ; ++it)
	{
		if (boost::regex_match((*it)->first->NameStr, e)) {
			destination.push_back(*it);
		}
	}
	return destination;
}

// ----------------------------------------------------------------------------

const_shared_strings PE::find_imports(const std::string& function_name_regexp,
									  const std::string& dll_name_regexp,
									  bool  case_sensitivity) const
{
	auto destination = boost::make_shared<std::vector<std::string> >();
	if (!_initialized) {
		return destination;
	}

	auto matching_dlls = _find_imported_dlls(dll_name_regexp);

	boost::regex e;
	if (case_sensitivity) {
		e = boost::regex(function_name_regexp);
	}
	else {
		e = boost::regex(function_name_regexp, boost::regex::icase);
	}

	// Iterate on matching DLLs
	for (auto it = matching_dlls.begin() ; it != matching_dlls.end() ; ++it)
	{
		// Iterate on functions imported by each of these DLLs
		for (auto it2 = (*it)->second.begin() ; it2 != (*it)->second.end() ; ++it2)
		{
			if ((*it2)->Name == "") { // Functions imported by ordinal are skipped.
				continue;
			}
			if (boost::regex_match((*it2)->Name, e)) {
				destination->push_back((*it2)->Name);
			}
		}
	}
	return destination;
}

} // !namespace mana
