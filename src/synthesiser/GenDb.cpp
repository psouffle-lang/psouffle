/**
 * Souffle - A Datalog Compiler
 * Copyright (c) 2022, The Souffle Developers. All rights reserved.
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file GenDb.cpp
 *
 * Implementation of the various C++ representations for classes, functions, etc.
 * and a way to produce C++ code.
 *
 ***********************************************************************/
#include "GenDb.h"
#include "souffle/utility/ContainerUtil.h"
#include "souffle/utility/FileUtil.h"
#include "souffle/utility/MiscUtil.h"
#include "souffle/utility/StreamUtil.h"
#include "souffle/utility/StringUtil.h"
#include "souffle/utility/json11.h"
#include "souffle/utility/tinyformat.h"
#include "synthesiser/GenDb.h"
#include <filesystem>
#include <list>
namespace fs = std::filesystem;

namespace souffle::synthesiser {

void GenFile::addDependency(GenFile& dep, bool def_only) {
    if (def_only) {
        dependencies.emplace(&dep);
    } else {
        decl_dependencies.emplace(&dep);
    }
}

void GenFile::addInclude(const std::string& str, bool def_only) {
    if (def_only) {
        includes.emplace(str);
    } else {
        decl_includes.emplace(str);
    }
}

void GenFunction::setRetType(std::string ty) {
    retType = ty;
}

void GenFunction::setNextArg(std::string ty, std::string name, std::optional<std::string> defaultValue) {
    args.push_back(std::make_tuple(ty, name, defaultValue));
}

void GenFunction::setNextInitializer(std::string field, std::string value) {
    initializer.push_back(std::make_pair(field, value));
}

void GenFunction::declaration(const GenDb*, std::ostream& o) const {
    o << retType << " " << name << "(" << join(args, ",", [&](auto& out, const auto arg) {
        out << std::get<0>(arg) << " " << std::get<1>(arg);
        std::optional<std::string> defaultValue = std::get<2>(arg);
        if (defaultValue) {
            out << " = " << *defaultValue;
        }
    }) << ");";
}

void GenFunction::definition(const GenDb*, std::ostream& o) const {
    o << retType << " ";
    if (cl) {
        o << cl->getName() << "::";
    }
    o << name << "(" << join(args, ",", [&](auto& out, const auto arg) {
        out << std::get<0>(arg) << " " << std::get<1>(arg);
    }) << ")";
    if (isConstructor && initializer.size() > 0) {
        o << ":\n";
        o << join(initializer, ",\n",
                [&](auto& out, const auto arg) { out << arg.first << "(" << arg.second << ")"; });
    }
    o << "{\n";
    o << bodyStream.str();
    o << "}\n";
}

GenFunction& GenClass::addFunction(std::string name, Visibility v) {
    methods.push_back(mk<GenFunction>(name, this, v));
    return *methods.back();
}

void GenClass::addField(std::string type, std::string name, Visibility v, std::optional<std::string> init) {
    fields.push_back(std::make_tuple(name, type, v, init));
}

GenFunction& GenClass::addConstructor(Visibility v) {
    methods.push_back(mk<GenFunction>(getName(), this, v));
    GenFunction& m = *methods.back();
    m.setIsConstructor();
    return m;
}

void GenClass::declaration(const GenDb* db, std::ostream& o) const {
    o << "namespace " << db->getNS() << " {\n";

    o << "using namespace souffle;\n";

    o << "class " << name;
    if (inheritance.size() > 0) {
        o << ": " << join(inheritance, ", ", [&](auto& out, const auto arg) { out << arg; });
    }
    o << " {\n";
    std::stringstream public_o;
    std::stringstream private_o;
    public_o << "public:\n";
    private_o << "private:\n";

    for (auto& fn : methods) {
        auto& o = (fn->getVisibility() == Public) ? public_o : private_o;
        fn->declaration(db, o);
        o << "\n";
    }
    for (auto& [field, ty, v, init] : fields) {
        auto& o = (v == Public) ? public_o : private_o;
        o << ty << " " << field;
        if (init) {
            o << *init;
        }
        o << ";\n";
    }
    o << public_o.str();
    o << private_o.str();
    o << "};\n";
    o << "} // namespace " << db->getNS() << "\n";
}

void GenClass::definition(const GenDb* db, std::ostream& o) const {
    if (ignoreUnusedArgumentWarning) {
        o << "#ifdef _MSC_VER\n";
        o << "#pragma warning(disable: 4100)\n";
        o << "#endif // _MSC_VER\n";
    }
    o << "namespace " << db->getNS() << " {\n";
    o << "using namespace souffle;\n";
    for (auto& fn : methods) {
        fn->definition(db, o);
        o << "\n";
    }
    o << "} // namespace " << db->getNS() << "\n";

    if (ignoreUnusedArgumentWarning) {
        // restore unused argument warning
        o << "#ifdef _MSC_VER\n";
        o << "#pragma warning(default: 4100)\n";
        o << "#endif // _MSC_VER\n";
    }
    o << hiddenHooksStream.str() << "\n";
}

void GenDb::setNS(std::string ns) {
    this->ns = ns;
}

std::string GenDb::getNS(const bool spaced) const {
    if (spaced) {
        return std::string{" "} + ns;
    } else {
        return ns;
    }
}

GenClass& GenDb::getClass(std::string name, fs::path basename) {
    classes.push_back(mk<GenClass>(name, basename));
    GenClass& res = *classes.back();
    nameToGen[name] = &res;
    return res;
}

GenDatastructure& GenDb::getDatastructure(
        std::string name, fs::path basename, std::optional<std::string> namespace_opt) {
    std::string fullName = name;
    if (namespace_opt) {
        fullName = *namespace_opt + "::" + name;
    }
    datastructures.push_back(mk<GenDatastructure>(name, basename, namespace_opt));
    GenDatastructure& res = *datastructures.back();
    nameToGen[fullName] = &res;
    return res;
}

void GenDatastructure::declaration(const GenDb* db, std::ostream& o) const {
    std::string ns = db->getNS(false);
    if (namespace_name) {
        ns += (ns.empty() ? "" : "::") + *namespace_name;
    }
    o << "namespace " << ns << " {\n";
    o << "using namespace souffle;\n";
    o << declarationStream.str();
    o << "} // namespace " << ns << " \n";
}

void GenDatastructure::definition(const GenDb* db, std::ostream& o) const {
    std::string ns = db->getNS(false);
    if (namespace_name) {
        ns += (ns.empty() ? "" : "::") + *namespace_name;
    }
    o << "namespace " << ns << " {\n";
    o << "using namespace souffle;\n";
    o << definitionStream.str();
    o << "} // namespace " << ns << " \n";
}

void GenDb::emitSingleFile(std::ostream& o) {
    std::set<std::string> defines = globalDefines;
    std::set<std::string> includes = globalIncludes;
    auto add = [&](GenFile& gen) {
        auto& inc = gen.getDeclIncludes();
        includes.insert(inc.begin(), inc.end());
        inc = gen.getIncludes();
        includes.insert(inc.begin(), inc.end());
    };

    for (auto& ds : datastructures) {
        add(*ds);
    }
    for (auto& cl : classes) {
        add(*cl);
    }

    for (auto& def : defines) {
        o << "#define " << def << "\n";
    }
    for (auto& inc : includes) {
        o << "#include " << inc << "\n";
    }
    o << "namespace functors {\n";
    o << "extern \"C\" {\n";
    o << externCStream.str();
    o << "}\n";
    o << "} //namespace functors\n";
    for (auto& ds : datastructures) {
        ds->declaration(this, o);
        ds->definition(this, o);
    }
    std::size_t size = classes.size();
    for (std::size_t i = 1; i < size - 1; i++) {
        Own<GenClass>& cl = classes[i];
        cl->declaration(this, o);
        cl->definition(this, o);
    }
    if (classes.size() > 1) {
        classes.front()->declaration(this, o);
        classes.front()->definition(this, o);
        classes.back()->declaration(this, o);
        classes.back()->definition(this, o);
    } else if (classes.size() == 1) {
        classes.front()->declaration(this, o);
        classes.front()->definition(this, o);
    }
}

std::string GenDb::emitMultipleFilesInDir(fs::path dir, std::vector<fs::path>& toCompile) {
    fs::path rootDir = dir;
    fs::create_directories(rootDir);

    auto globalHeader = [&](std::ofstream& hpp) {
        for (auto& def : globalDefines) {
            hpp << "#define " << def << "\n";
        }
        for (auto& inc : globalIncludes) {
            hpp << "#include " << inc << "\n";
        }
    };

    auto genHeader = [&](std::ofstream& hpp, std::ofstream& cpp, GenFile& gen) {
        hpp << "#pragma once\n";
        globalHeader(hpp);
        for (const std::string& inc : gen.getSortedDeclIncludes()) {
            hpp << "#include " << inc << "\n";
        }
        for (GenFile* dep : gen.getSortedDeclDependencies()) {
            hpp << "#include " << dep->getHeader() << "\n";
        }
        for (const std::string& inc : gen.getSortedIncludes()) {
            cpp << "#include " << inc << "\n";
        }
        for (GenFile* dep : gen.getSortedDependencies()) {
            cpp << "#include " << dep->getHeader() << "\n";
        }
        cpp << "#include " << gen.getHeader() << "\n";
    };

    for (auto& ds : datastructures) {
        toCompile.push_back(rootDir / ds->fileBaseName().concat(".cpp"));
        std::ofstream hpp{rootDir / ds->fileBaseName().concat(".hpp")};
        std::ofstream cpp{rootDir / ds->fileBaseName().concat(".cpp")};
        genHeader(hpp, cpp, *ds);
        ds->declaration(this, hpp);
        ds->definition(this, cpp);
    }
    std::string mainClass;
    for (auto& cl : classes) {
        toCompile.push_back(rootDir / cl->fileBaseName().concat(".cpp"));
        std::ofstream hpp{rootDir / cl->fileBaseName().concat(".hpp")};
        std::ofstream cpp{rootDir / cl->fileBaseName().concat(".cpp")};
        genHeader(hpp, cpp, *cl);
        cl->declaration(this, hpp);
        if (cl->isMain) {
            mainClass = cl->getName();
            cpp << "namespace functors {\n";
            cpp << "extern \"C\" {\n";
            cpp << externCStream.str() << "\n";
            cpp << "}\n";
            cpp << "} //namespace functors\n";
        }
        cl->definition(this, cpp);
    }
    return mainClass;
}

}  // namespace souffle::synthesiser
