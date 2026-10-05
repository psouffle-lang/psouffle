/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2022, The Souffle Developers. All rights reserved.
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file GenDb.h
 *
 * Provides some classes to represent C++ classes, functions, etc.
 * This permits as a little more structured way to generate the C++ code
 * instead of only relying on std::ostream in the Synthesiser.
 ***********************************************************************/

#pragma once

#include "souffle/utility/Types.h"
#include <algorithm>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace souffle::synthesiser {

class GenDb;

class GenFile {
public:
    GenFile(fs::path basename) : basename(std::move(basename)) {}

    /*
     * Basename of the file (without extension) where
     * that should be produced for this code.
     */
    virtual fs::path fileBaseName() const {
        return basename;
    };

    fs::path getHeader() const {
        fs::path header = basename;
        return header.concat(".hpp");
    }

    /*
     * Sets 'dep' as a class that is used by the current construct
     * and must be #include in the current construct
     * Set 'def_only' to true if only the .cpp file must include it.
     */
    void addDependency(GenFile& dep, bool def_only = false);

    /*
     * #include 'str' must be included when generating the code
     */
    void addInclude(const std::string& str, bool def_only = false);

    /**
     * Accessors for private members
     */
    std::set<std::string>& getDeclIncludes() {
        return decl_includes;
    }
    std::vector<std::string> getSortedDeclIncludes() {
        std::vector<std::string> v{decl_includes.begin(), decl_includes.end()};
        std::sort(v.begin(), v.end());
        return v;
    }

    std::set<std::string>& getIncludes() {
        return includes;
    }
    std::vector<std::string> getSortedIncludes() {
        std::vector<std::string> v{includes.begin(), includes.end()};
        std::sort(v.begin(), v.end());
        return v;
    }

    std::set<GenFile*>& getDeclDependencies() {
        return decl_dependencies;
    }
    std::vector<GenFile*> getSortedDeclDependencies() {
        std::vector<GenFile*> v{decl_dependencies.begin(), decl_dependencies.end()};
        std::sort(v.begin(), v.end(),
                [](const GenFile* a, const GenFile* b) { return a->getHeader() < b->getHeader(); });
        return v;
    }

    std::set<GenFile*>& getDependencies() {
        return dependencies;
    }
    std::vector<GenFile*> getSortedDependencies() {
        std::vector<GenFile*> v{dependencies.begin(), dependencies.end()};
        std::sort(v.begin(), v.end(),
                [](const GenFile* a, const GenFile* b) { return a->getHeader() < b->getHeader(); });
        return v;
    }

private:
    fs::path basename;

    std::set<std::string> decl_includes;
    std::set<GenFile*> decl_dependencies;
    std::set<std::string> includes;
    std::set<GenFile*> dependencies;
};

/**
 * Object representing some C++ construct that should be emitted in the
 * generated code. For instance, a class, a function...
 */
class Gen {
public:
    Gen(std::string name) : name(name) {}

    /* Emit the declaration of this construct in C++,
     * typically what we would expect from a .hpp file
     */
    virtual void declaration(const GenDb* db, std::ostream& o) const = 0;

    /* Emit the Implementation of this construct in C++,
     * typically what we would expect from a .cpp file
     */
    virtual void definition(const GenDb* db, std::ostream& o) const = 0;

    std::string& getName() {
        return name;
    }

protected:
    std::string name;
};

class GenClass;
class GenDb;

/**
 * Visibility of the elements in the class
 */
enum Visibility { Public = 0, Private };

/**
 * Class helper to manipulate/build a function to be emitted
 * by the C++ Synthesizer.
 */
class GenFunction : public Gen {
public:
    GenFunction(std::string name, GenClass* cl, Visibility v)
            : Gen(name), cl(cl), visibility(v), override(false) {}

    virtual ~GenFunction() = default;

    void setRetType(std::string ty);
    void setNextArg(std::string ty, std::string name, std::optional<std::string> defaultValue = std::nullopt);
    void setNextInitializer(std::string name, std::string value);

    void setIsConstructor() {
        isConstructor = true;
    };
    void setOverride() {
        override = true;
    };

    void declaration(const GenDb* db, std::ostream& o) const override;

    void definition(const GenDb* db, std::ostream& o) const override;

    Visibility getVisibility() {
        return visibility;
    }

    std::ostream& body() {
        return bodyStream;
    }

private:
    GenClass* cl;
    Visibility visibility;
    bool isConstructor = false;
    bool override;
    std::string retType;
    std::vector<std::tuple<std::string, std::string, std::optional<std::string>>> args;
    std::vector<std::pair<std::string, std::string>> initializer;
    std::stringstream bodyStream;
};

/**
 * Class helper to manipulate/build a class to be emitted
 * by the C++ Synthesizer.
 */
class GenClass : public Gen, public GenFile {
public:
    GenClass(std::string name, fs::path basename) : Gen(name), GenFile(basename) {}
    virtual ~GenClass() = default;

    GenFunction& addFunction(std::string name, Visibility);
    GenFunction& addConstructor(Visibility);

    void addField(
            std::string type, std::string name, Visibility, std::optional<std::string> init = std::nullopt);

    void declaration(const GenDb* db, std::ostream& o) const override;

    void definition(const GenDb* db, std::ostream& o) const override;

    void inherits(std::string parent) {
        inheritance.push_back(parent);
    }

    bool ignoreUnusedArgumentWarning = false;
    bool isMain = false;

    std::ostream& hooks() {
        return hiddenHooksStream;
    }

private:
    std::vector<Own<GenFunction>> methods;
    std::vector<std::tuple<std::string /*name*/, std::string /*type*/, Visibility,
            std::optional<std::string> /* initializer value */
            >>
            fields;
    std::vector<std::string> inheritance;
    std::stringstream hiddenHooksStream;
};

/**
 * Class helper to manipulate/build the class
 * for one of the Souffle specialized datastructures
 * (e.g. BTree, BTreeDelete, Brie, etc.)
 */
class GenDatastructure : public Gen, public GenFile {
public:
    GenDatastructure(std::string name, fs::path basename, std::optional<std::string> namespace_opt)
            : Gen(name), GenFile(basename), namespace_name(namespace_opt) {}
    virtual ~GenDatastructure() = default;

    std::ostream& decl() {
        return declarationStream;
    }
    std::ostream& def() {
        return definitionStream;
    }

    void declaration(const GenDb* db, std::ostream& o) const override;
    void definition(const GenDb* db, std::ostream& o) const override;

private:
    std::optional<std::string> namespace_name;
    std::stringstream declarationStream;
    std::stringstream definitionStream;
};

/**
 * Class that stores all the constructs build by the Synthesizer.
 * Provides methods to emit the generated C++ code to a unique
 * or to multiple files.
 */
class GenDb {
public:
    void setNS(std::string ns);

    std::string getNS(const bool spaced = true) const;

    GenClass& getClass(std::string name, fs::path basename);
    GenDatastructure& getDatastructure(
            std::string name, fs::path basename, std::optional<std::string> namespace_opt);

    void emitSingleFile(std::ostream& o);
    std::string emitMultipleFilesInDir(fs::path dir, std::vector<fs::path>& toCompile);

    std::ostream& externC() {
        return externCStream;
    }

    void addGlobalInclude(std::string str) {
        globalIncludes.emplace(str);
    }
    void addGlobalDefine(std::string str) {
        globalDefines.emplace(str);
    }

    void usesDatastructure(GenClass& cl, std::string str) {
        if (nameToGen.count(str)) {
            cl.addDependency(*nameToGen[str]);
        } else if (nameToInclude.count(str)) {
            cl.addInclude(nameToInclude[str]);
        }
    }

    void datastructureIncludes(std::string datastructure, std::string inc) {
        nameToInclude[datastructure] = inc;
    }

private:
    // namespace of the generated code
    std::string ns = "";
    std::vector<Own<GenDatastructure>> datastructures;
    std::vector<Own<GenClass>> classes;

    std::map<std::string, GenFile*> nameToGen;
    std::map<std::string, std::string> nameToInclude;

    std::stringstream externCStream;

    std::set<std::string> globalIncludes;
    std::set<std::string> globalDefines;
};

}  // namespace souffle::synthesiser
