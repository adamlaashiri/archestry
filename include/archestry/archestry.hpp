#ifndef ARCHESTRY_ARCHESTRY_H
#define ARCHESTRY_ARCHESTRY_H

#ifdef ARCHESTRY_DEBUG
#include <iostream>
#include <cstdlib>
#endif

#include <stdint.h>
#include <bit>
#include <memory>
#include <limits>
#include <array>
#include <vector>
#include <unordered_map>
#include <type_traits>
#include <tuple>
#include <optional>
#include <utility>

#if defined(_MSC_VER)
	#define ARCH_FORCEINLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
	#define ARCH_FORCEINLINE inline __attribute__((always_inline))
#else
	#define ARCH_FORCEINLINE inline
#endif


#ifdef ARCHESTRY_DEBUG
	#define ARCH_ASSERT(expr, msg) if (!(expr)) { std::cerr << "[archestry error] (" << __func__ << "): " << msg << '\n'; std::abort(); }
	#define ARCH_MESSAGE(msg) std::cout << msg << '\n'
#else
	#define ARCH_ASSERT(expr, msg) ((void)0)
	#define ARCH_MESSAGE(msg) ((void)0)
#endif


namespace archestry {

	// An EntityID is just a number used
	// to identify and group components
	using EntityID = size_t;


	constexpr EntityID NULL_ENTITY = std::numeric_limits<EntityID>::max();


	/*
	* Each component has its unique bit.
	* The first bit is reserved to represent the entity's state.
	* A combination of component bits form an archetype mask.
	*/
	using Bitmask = uint64_t;


	constexpr size_t MAX_COMPONENT_TYPE_COUNT = sizeof(Bitmask) * CHAR_BIT - 1;
	constexpr size_t INIT_POOL_CAPACITY = 64;
	

	// First bit representing an entity's state
	constexpr Bitmask INACTIVE_ENTITY = 0;
	constexpr Bitmask ACTIVE_ENTITY = 1;


	using ConstructFn = void(*)(void* dst, void* src);
	using AssignFn = void(*)(void* dst, void* src);
	using DestructFn = void(*)(void* src);


	// Stores component metadata
	struct ComponentInfo {
		size_t Size;
		size_t Alignment;
		bool IsTrivial;
		ConstructFn MoveCtor;
		AssignFn MoveAssign;
		DestructFn Destruct;
	};


	/*
	* Handles registration and management of component types
	* and their associated metadata
	*/
	class ComponentRegistry {
#pragma region Asserts
#define ASSERT_VALID_MASK(mask) ARCH_ASSERT(s_Registry.find(mask) != s_Registry.end(), "Type with mask " << mask << " is not registered");
#define ASSERT_COMPONENT_COUNT() ARCH_ASSERT(s_Counter <= MAX_COMPONENT_TYPE_COUNT, "Attempting to register more than " << MAX_COMPONENT_TYPE_COUNT << " components");
#pragma endregion

	public:
		template<typename T>
		static const Bitmask GetMask() {
			// Cache mask once
			static const Bitmask mask = RegisterType<T>();
			return mask;
		}

		static const ComponentInfo& GetInfo(Bitmask mask) {
			ASSERT_VALID_MASK(mask);
			return s_Registry[mask];
		}
	private:
		friend class Registry;

		inline static uint64_t s_Counter = 0;
		inline static std::unordered_map<Bitmask, ComponentInfo> s_Registry;

		template<typename T>
		static const Bitmask RegisterType() {
			ASSERT_COMPONENT_COUNT();

			/*
			* The component pool requires at least one of the two copy methods
			* to efficiently and safely resize its buffer and move components.
			* Non-trivial types must be both moveable and destructible
			*/

			// Trivial != trivially copyable, having user defined copy/move constructors
			// or destructors (including its members) means the type cannot be safely memcopied.
			// Custom ordinary constructors have no affect on this.

			// Trivial structures have no user defined constructors (any kind), destructor,
			// or virtual functions (applies to all members recursively)

			static_assert(
				std::is_trivially_copyable_v<T> || std::is_nothrow_move_constructible_v<T>,
				"Component must be trivially copyable or have a non-throwing move constructor"
				);

			if constexpr (!std::is_trivially_copyable_v<T>) {
				static_assert(std::is_nothrow_destructible_v<T>, "Non-trivial component must be non-throwing destructible");
				static_assert(std::is_nothrow_move_assignable_v<T>, "Non-trivial component must be non-throwing move assignable");
			}

			// We bitshift from 2, because the first bit is reserved for the state of an entity
			Bitmask mask = 2ULL << s_Counter++;

			s_Registry.try_emplace(
				mask,
				sizeof(T),
				alignof(T),
				std::is_trivially_copyable_v<T>,
				!std::is_trivially_copyable_v<T> ? &MoveConstruct<T> : nullptr,
				!std::is_trivially_copyable_v<T> ? &MoveAssign<T> : nullptr,
				!std::is_trivially_copyable_v<T> ? &Destruct<T> : nullptr
			);

			return mask;
		}

		template<typename T>
		static void Construct(void* dst, const void* src) {
			new (dst) T(*static_cast<T*>(src));
		}

		template<typename T>
		static void MoveConstruct(void* dst, void* src) {
			new (dst) T(std::move(*static_cast<T*>(src)));
		}

		template<typename T>
		static void MoveAssign(void* dst, void* src) {
			*static_cast<T*>(dst) = std::move(*static_cast<T*>(src));
		}

		template<typename T>
		static void Destruct(void* src) {
			static_cast<const T*>(src)->~T();
		}
	};


	/*
	* Iterates over the set bits in a bitmask
	* (e.g., component bits in an archetype)
	*/
	struct BitmaskIterator {
		Bitmask Mask = 0;

		BitmaskIterator(Bitmask mask) : Mask(mask) {}

		bool HasNext() const {
			return Mask != 0;
		}

		Bitmask Next() {
			ARCH_ASSERT(Mask > 0, "Iterator Mask is 0");
			// Get next set least significant bit
			const Bitmask bit = Mask & -Mask;
			// Clear last bit to get to the next set bit
			Mask ^= bit;
			return bit;
		}
	};


	// Utillity to convert a single bit component mask to an index
	constexpr size_t ComponentIndex(Bitmask mask) {
		ARCH_ASSERT(std::popcount(mask) == 1, "Not a single bit mask");
		return std::countr_zero(mask) - 1;
	}


	// Utillity to combine multiple component bits into one mask
	template<typename ...Components>
	constexpr Bitmask CombineComponents() {
		return (ComponentRegistry::GetMask<Components>() | ... | 0);
	}


	/*
	* Type erased pool that stores and
	* manages components contiguously in memory
	*/
	class ComponentPool {
#pragma region Asserts
#define	ASSERT_INVALID_COPY_TYPE() ARCH_ASSERT(false, "Copy type not supported");
#pragma endregion

	private:
		// Self contained aligned buffer that is only
		// meant to be used inside this pool
		class Buffer {
		private:
			void* m_pBase = nullptr;
			void* m_pAligned = nullptr;

			void Free() {
				std::free(m_pBase);
				m_pBase = nullptr;
				m_pAligned = nullptr;
			}

		public:
			Buffer() = delete;

			Buffer(size_t size, size_t alignment) {
				ARCH_ASSERT(alignment != 0 && (alignment & (~alignment)) == 0,
					"Alignment must be non-zero and a power of 2");

				m_pBase = std::malloc(size + alignment - 1);

				if (!m_pBase) { throw std::bad_alloc(); }

				// Round pointer up to nearest alignment multiple, if not already aligned
				m_pAligned = reinterpret_cast<void*>(
					((reinterpret_cast<uintptr_t>(m_pBase) + alignment - 1) & ~(alignment - 1))
				);
			}

			Buffer(const Buffer& other) = delete;

			Buffer(Buffer&& other) noexcept :
				m_pBase(other.m_pBase),
				m_pAligned(other.m_pAligned) {
				other.m_pBase = nullptr;
				other.m_pAligned = nullptr;
			}

			~Buffer() {
				Free();
			}

			Buffer& operator = (const Buffer& other) = delete;

			Buffer& operator = (Buffer&& other) noexcept {
				if (this == &other)
					return *this;
				Free();
				m_pBase = other.m_pBase;
				m_pAligned = other.m_pAligned;
				other.m_pBase = nullptr;
				other.m_pAligned = nullptr;

				return *this;
			}

			void* operator[](size_t byteOffset) {
				return reinterpret_cast<void*>(
					reinterpret_cast<uintptr_t>(m_pAligned) + byteOffset
					);
			}

			const void* operator[](size_t byteOffset) const {
				return reinterpret_cast<void*>(
					reinterpret_cast<uintptr_t>(m_pAligned) + byteOffset
					);
			}
		};

		// Copy type determines the most efficient
		// method to move components between pools
		enum class CopyType {
			Memcpy, // memcpy
			Move // Move constructor & move assignment
		};

		const ComponentInfo m_ComponentInfo;

		const CopyType m_CopyType;

		Buffer m_Buffer;

		size_t m_Capacity = 0;

		size_t m_Size = 0;

		// Replace component at index a with component at index b
		void Replace(size_t a, size_t b) {
			void* aSrc = (*this)[a];
			void* bSrc = (*this)[b];

			switch (m_CopyType) {
			case CopyType::Memcpy:
				memcpy(aSrc, bSrc, m_ComponentInfo.Size);
				break;
			case CopyType::Move:
				m_ComponentInfo.MoveAssign(aSrc, bSrc);
				m_ComponentInfo.Destruct(bSrc);
				break;
			default:
				ASSERT_INVALID_COPY_TYPE();
			}
		}

		void Resize(size_t factor) {
			const size_t newCapacity = 1 + factor * m_Capacity;
			Buffer newBuffer(newCapacity * m_ComponentInfo.Size, m_ComponentInfo.Alignment);

			switch (m_CopyType) {
			case CopyType::Memcpy: {
				/*
				* Copy all components from the start of the current buffer
				* to the start of the new buffer.
				*/
				void* dst = newBuffer[0];
				void* src = (*this)[0];
				memcpy(dst, src, m_Size * m_ComponentInfo.Size);
				break;
			}
			case CopyType::Move: {
				/*
				* Invoke the move constructor for each element and move it
				* to its corresponding index in the new buffer.
				* Then destroy the old element.
				*/
				const size_t size = m_Size;
				for (size_t i = 0; i < size; i++) {
					void* dst = newBuffer[i * m_ComponentInfo.Size];
					void* src = (*this)[i];
					m_ComponentInfo.MoveCtor(dst, src);
					m_ComponentInfo.Destruct(src);
				}
				break;
			}
			default:
				ASSERT_INVALID_COPY_TYPE();
			};

			m_Capacity = newCapacity;

			// Free old buffer and replace it with the new bigger one
			m_Buffer = std::move(newBuffer);
		}

		void EnsureSize() {
			if (m_Size >= m_Capacity)
				Resize(2);
		}

	public:
		ComponentPool() = delete;

		ComponentPool(ComponentInfo info, size_t capacity) :
			m_ComponentInfo(info),
			m_CopyType(m_ComponentInfo.IsTrivial ? CopyType::Memcpy : CopyType::Move),
			m_Buffer(capacity* m_ComponentInfo.Size, m_ComponentInfo.Alignment),
			m_Capacity(capacity),
			m_Size(0) {
			// ASSERT valid componentInfo
			/*
			// For copy types, trivial copying (using memcpy)
			// is the most favourable and has precedence over move constructors
			*/
		}

		ComponentPool(const ComponentPool& other) = delete;

		ComponentPool(ComponentPool&& other) = delete;

		ComponentPool& operator=(const ComponentPool& other) = delete;

		ComponentPool& operator=(ComponentPool&& other) = delete;

		~ComponentPool() {
			if (m_CopyType == CopyType::Move)
				for (size_t i = 0; i < m_Size; i++)
					m_ComponentInfo.Destruct((*this)[i]);
		}

		// Construct component in-place
		template<typename Component, typename... Args>
		Component& Emplace(Args&&... args) {
			EnsureSize();

			void* dst = m_Buffer[m_Size * m_ComponentInfo.Size];
			new (dst) Component(std::forward<Args>(args)...);

			m_Size++;

			return *static_cast<Component*>(dst);
		}

		// Type erased component addition
		// through memcpy / move semantics
		void* Add(void* component) {
			EnsureSize();

			void* dst = m_Buffer[m_Size * m_ComponentInfo.Size];

			switch (m_CopyType) {
			case CopyType::Memcpy:
				memcpy(dst, component, m_ComponentInfo.Size);
				break;
			case CopyType::Move:
				m_ComponentInfo.MoveCtor(dst, component);
				break;
			default:
				ASSERT_INVALID_COPY_TYPE();
			};

			m_Size++;

			return dst;
		}

		void Delete(size_t index) {
			// We skip the replacement if the component is the last in the pool
			if (index != m_Size - 1)
				/*
				* replace the component to be deleted with the last component and then delete it
				* to maintain continuous and tightly packed memory
				*/
				Replace(index, m_Size - 1);
			else if (!m_ComponentInfo.IsTrivial)
				m_ComponentInfo.Destruct((*this)[m_Size - 1]);

			m_Size--;
		}

		size_t GetSize() const {
			return m_Size;
		}

		/*
		* returns void* to a component at the given index.
		* Components are guaranteed to be tighly packed consecutively in memory,
		* each occupying ComponentInfo.Size bytes
		*/
		void* operator[](size_t index) {
			ARCH_ASSERT(index < m_Size, "Index " << index << " out of range (" << m_Size << ")");

			return m_Buffer[index * m_ComponentInfo.Size];
		}
	};


	// Groups entities with the same set of components together.
	// Not standalone, owned and managed by parent Registry.
	// Requires valid entityToIndex reference from Registry.
	// entityToIndex[entity] -> index in component pool(s)
	// indexToEntity[index] -> entity at index in component pool(s)
	// Invariants:
	// - All component pools have the same length.
	// - entityToIndex[entity] matches position in pool(s).
	// - Always contains at least one entity (new archertypes are immediately populated & empty ones are destroyed).
	// - Registry guarantees all input EntityIDs and components belong to this archetype.
	// - Validations happens upstream, no need to assert entity presence or component validity.
	// - Mutations of entityToIndex only affect entities belonging to this archetype (Registry guarantees).
	class Archetype {
#pragma region Asserts
#define ASSERT_POOLS_SYNCED() ARCH_ASSERT(ArePoolsSynced(), "Component pools out of sync.");
#pragma endregion

	private:
		const Bitmask m_ArchetypeMask = 0;

		EntityID m_LastAddedEntity = 0;

		// Tracks which components are still 
		// expected to be added for last added entity
		Bitmask m_PendingMask = 0;

		size_t m_Size = 0;

		std::vector<EntityID> m_IndexToEntity;

		// TODO: Use referene wrapper
		std::vector<size_t>& m_EntityToIndex;

		// Sparse array for O(1) lookup.
		// Scales with max component type count.
		std::array<std::unique_ptr<ComponentPool>, MAX_COMPONENT_TYPE_COUNT> m_Pools;

		// Ensures a new entity is registered once.
		// Following Emplace/Adds calls attach components to the same entity.
		void EnsureEntity(EntityID ID, Bitmask componentMask) {
			if (m_PendingMask == 0) {
				// New entity
				ASSERT_POOLS_SYNCED();

				m_PendingMask = m_ArchetypeMask;
				m_LastAddedEntity = ID;
				m_IndexToEntity.push_back(ID);
			}

			// Components are added sequentially for the same entity

			m_PendingMask &= ~componentMask;

			// During Move, if the new archetype updates entityToIndex immediately,
			// old archetype can't find the entity to remove it.
			// Delay enityToIndex update until component addition is complete
			if (m_PendingMask == 0)
				m_EntityToIndex[ID] = m_Size++;
		}

		void UpdateRemovedIndex(size_t index) {
			/*
			* Entity was already last in the pool; no replacement performed.
			* ComponentPool index mapping remains valid, no reassignment of index required.
			*/
			if (index == m_Size - 1) {
				m_IndexToEntity.pop_back();
				m_Size--;
				return;
			}

			// Replacement occured, update index of moved entity
			m_EntityToIndex[m_IndexToEntity[m_Size - 1]] = index;
			m_IndexToEntity.pop_back();
			m_Size--;
		}

		// For debugging, Check that all Pools lengths match
		bool ArePoolsSynced() const {
			for (BitmaskIterator it{ m_ArchetypeMask }; it.HasNext();)
				if (m_Size != m_Pools[ComponentIndex(it.Next())]->GetSize())
					return false;

			return true;
		}

		template<typename Component>
		ARCH_FORCEINLINE ComponentPool& GetPool() {
			static const auto index = ComponentIndex(
				ComponentRegistry::GetMask<Component>());
			return *m_Pools[index];
		}

	public:
		Archetype() = delete;

		Archetype(Bitmask archetypeMask, std::vector<size_t>& entityToIndex) :
			m_ArchetypeMask(archetypeMask),
			m_EntityToIndex(entityToIndex) {
			for (BitmaskIterator it{ archetypeMask }; it.HasNext();) {
				const Bitmask componentMask = it.Next();
				m_Pools[ComponentIndex(componentMask)] = 
					std::make_unique<ComponentPool>(
						ComponentRegistry::GetInfo(componentMask), INIT_POOL_CAPACITY
					);
			}
		}

		template<typename Component, typename... Args>
		Component& Emplace(EntityID ID, Args&&... args) noexcept {
			EnsureEntity(ID, ComponentRegistry::GetMask<Component>());
			return GetPool<Component>().Emplace<Component>(std::forward<Args>(args)...);
		}

		template<typename Component>
		Component& Add(EntityID ID, Component&& component) {
			EnsureEntity(ID, ComponentRegistry::GetMask<Component>());
			return *static_cast<Component*>(GetPool<Component>().Add(&component));
		}

		template<typename ...Components>
		std::tuple<Components&...> AddMultiple(EntityID ID, Components&&... components) {
			EnsureEntity(ID, CombineComponents<Components...>());
			return std::tuple<Components&...> {
				*static_cast<Components*>(GetPool<Components>().Add(&components))...
			};
		}

		template<typename Component>
		Component& Get(EntityID ID) {
			ASSERT_POOLS_SYNCED();
			return *static_cast<Component*>(GetPool<Component>()[m_EntityToIndex[ID]]);
		}

		template<typename ...Components>
		std::tuple<Components&...> Multiple(EntityID ID) {
			ASSERT_POOLS_SYNCED();
			const size_t entityIndex = m_EntityToIndex[ID];

			return std::tuple<Components&...>{
				*static_cast<Components*>(GetPool<Components>()[entityIndex])...
			};
		}

		template<typename ...Components>
		std::tuple<Components&...> First() {
			ASSERT_POOLS_SYNCED();
			return std::tuple<Components&...> {
				*static_cast<Components*>(GetPool<Components>()[0])...
			};
		}

		template<typename ...Components, typename Fn>
		void ForEach(Fn&& fn) {
			ASSERT_POOLS_SYNCED();

			// Cache pool base pointers before iteration
			// to avoid multiple pointer indirections
			auto typedPools = std::make_tuple(
				static_cast<Components*>(GetPool<Components>()[0])...
			);

			std::apply([&](auto... ptrs) {
				const size_t size = m_Size; // Micro-opt, Prevent reloads

				// constexpr evaluated at compile time to determines the right branch for provided lambda
				// This branch is for [](EntityID ID, Component& c1, Component& c2...);
				if constexpr (std::is_invocable_v<Fn&&, EntityID, Components&...>) {
					EntityID* indexToEntity = m_IndexToEntity.data();

					for (size_t i = 0; i < size; i++)
						fn(indexToEntity[i], ptrs[i]...);
				}
				// This branch is for [](Component& c1, Component& c2...);
				else if constexpr (std::is_invocable_v<Fn&&, Components&...>) {
					for (size_t i = 0; i < size; i++)
						fn(ptrs[i]...);
				}
				else
					ARCH_ASSERT(false,
						"Bad lambda provided to .ForEach, parameter pack does not match lambda args."
					);

				}, typedPools);
		}

		/*
		* Used in a type erased context.
		* Move every relevant component belonging
		* to given entity, to the other archetype,
		* and delete entity and all its existing
		* components in this archetype.
		*/
		void Move(EntityID ID, Archetype& other) {
			ASSERT_POOLS_SYNCED();

			const size_t entityIndex = m_EntityToIndex[ID];

			for (BitmaskIterator it{ m_ArchetypeMask }; it.HasNext();) {
				const Bitmask componentMask = it.Next();
				const Bitmask componentIndex = ComponentIndex(componentMask);

				if (other.m_ArchetypeMask & componentMask) {
					other.EnsureEntity(ID, componentMask);
					other.m_Pools[componentIndex]->Add((*m_Pools[componentIndex])[entityIndex]);
				}

				m_Pools[componentIndex]->Delete(entityIndex);
			}

			ARCH_ASSERT(other.ArePoolsSynced(),
				"Other archetype component pools out of sync.");

			// m_EntityToIndex[ID] now reflects the other archetype

			UpdateRemovedIndex(entityIndex);
		}

		void Delete(EntityID ID) {
			ASSERT_POOLS_SYNCED();

			const size_t index = m_EntityToIndex.at(ID);

			for (BitmaskIterator it{ m_ArchetypeMask }; it.HasNext();)
				m_Pools.at(ComponentIndex(it.Next()))->Delete(index);

			UpdateRemovedIndex(index);
		}

		bool IsEmpty() const {
			ASSERT_POOLS_SYNCED();
			return m_Size == 0;
		}

		size_t GetSize() const {
			ASSERT_POOLS_SYNCED();
			return m_Size;
		}
	};


	/*
	* Provides self contained queries defined
	* by the passed-in component parameter pack.
	*/
	template<typename ...Components>
	class Query {
#pragma region Asserts
#define ASSERT_VALID_REGISTRY() ARCH_ASSERT(m_Registry != nullptr, "Invalid pointer to Registry");
#pragma endregion

	private:
		Registry* m_Registry = nullptr;

		inline static Bitmask s_ComponentMask = CombineComponents<Components...>();

		Bitmask m_ExcludedComponentMask = 0;

		// Target all archetypes that contain the specified components
		// Exclude archetypes that contain components to be excluded
		bool IsTarget(Bitmask mask) const {
			if ((mask & s_ComponentMask) != s_ComponentMask ||
				(mask & m_ExcludedComponentMask) > 0)
				return false;

			return true;
		}

	public:
		Query(Registry* ecs) :
			m_Registry(ecs) {
		}

		// Excludes archetypes that contain any of the
		// components in the passed-in parameter pack.
		template<typename ...Components>
		Query& Without() {
			m_ExcludedComponentMask |= CombineComponents<Components...>();
			return *this;
		}
		
		bool HasEntity(EntityID ID) const {
			ASSERT_VALID_REGISTRY();
			ARCH_ASSERT(m_Registry->IsValidEntity(ID),
				"Entity with ID " << ID << " does not exist");

			if (s_ComponentMask == 0)
				return false;
			
			return m_Registry->template HasAllComponents<Components...>(ID) &&
				((m_Registry->m_Entities[ID] & m_ExcludedComponentMask) == 0);
		}

		// Return the first found set of components
		std::optional<std::tuple<Components&...>> GetFirst() {
			ASSERT_VALID_REGISTRY();

			if (s_ComponentMask == 0)
				return std::nullopt;

			for (auto& [mask, archetype] : m_Registry->m_Types) {
				if (!IsTarget(mask))
					continue;
				return archetype.First<Components...>();
			}

			return std::nullopt;
		}

		template<typename Fn>
		void ForEach(Fn&& fn) {
			ASSERT_VALID_REGISTRY();

			if (s_ComponentMask == 0)
				return;

			for (auto& [mask, archetype] : m_Registry->m_Types) {
				if (!IsTarget(mask))
					continue;

				archetype.ForEach<Components...>(fn);
			}
		}
	};


	// Main orchestrator of the ECS
	class Registry {
#pragma region Asserts
#define ASSERT_VALID_ENTITY(ID) ARCH_ASSERT(IsValidEntity(ID), "Entity with ID " << ID << " does not exist");
#define ASSERT_VALID_ARCHETYPE(mask) ARCH_ASSERT(m_Types.find(mask) != m_Types.end(), "Attempting to access inactive archetype: " << mask);
#define ASSERT_ENTITY_STATE_BIT_NOT_SET(mask) ARCH_ASSERT((mask & ACTIVE_ENTITY) == 0, "Entity state bit set.");
#pragma endregion

	private:
		template<typename...>
		friend class Query;

		EntityID m_MaxID = 0;

		/*
		* The entityMask encodes which archetype
		* an entity belongs to, based on its combination of components.
		* The first bit is reserved for storing the entity's state
		* and is ignored when querying archetypes.
		*/
		std::vector<Bitmask> m_Entities;

		// Stores IDs of deleted entities for later reuse.
		std::vector<EntityID> m_IdCache;

		// O(1) entity -> index lookup.
		// Scales with max entity ID.
		std::vector<size_t> m_EntityToIndex;

		// Container for all archetypes.
		std::unordered_map<Bitmask, Archetype> m_Types;

		bool IsValidEntity(EntityID ID) const {
			return ID != NULL_ENTITY &&
				ID < m_MaxID &&
				m_Entities[ID] != INACTIVE_ENTITY;
		}

		// Returns (and creates if needed) the archetype for the given mask.
		// Gatekeeper that guarantees valid archetype for given mask.
		Archetype& EnsureArchetype(Bitmask mask) {
			ASSERT_ENTITY_STATE_BIT_NOT_SET(mask);
			return m_Types.try_emplace(mask, mask, m_EntityToIndex).first->second;
		}

		// Enforce destroyed empty archetypes invariant
		void DestroyArchetypeIfEmpty(Bitmask mask, Archetype& arch) {
			if (arch.IsEmpty())
				m_Types.erase(mask);
		}

	public:
		Registry() = default;

		EntityID CreateEntity() {
			EntityID ID = 0;
			if (m_IdCache.size()) {
				ID = m_IdCache.back();
				m_IdCache.pop_back();
				m_Entities[ID] = ACTIVE_ENTITY;
			}
			else {
				ID = m_MaxID++;
				m_Entities.push_back(ACTIVE_ENTITY);
				m_EntityToIndex.push_back(0);
			}
			return ID;
		}

		void DeleteEntity(EntityID ID) {
			ASSERT_VALID_ENTITY(ID);

			Bitmask& entityMask = m_Entities[ID];
			const Bitmask archMask = entityMask & ~ACTIVE_ENTITY;

			// Destroy associated components
			if (archMask) {
				auto& arch = m_Types.at(archMask);
				arch.Delete(ID);
				DestroyArchetypeIfEmpty(archMask, arch);
			}
			entityMask = INACTIVE_ENTITY;
			m_IdCache.push_back(ID);
		}

		template<typename Component, typename... Args>
		Component& AddComponent(EntityID ID, Args&&... args) {
			ASSERT_VALID_ENTITY(ID);
			ARCH_ASSERT(!HasComponent<Component>(ID),
				"Entity already has the specified component.");

			Bitmask& entityMask = m_Entities[ID];
			const Bitmask componentMask = ComponentRegistry::GetMask<Component>();
			const Bitmask oldArchMask = entityMask & ~ACTIVE_ENTITY;
			const Bitmask newArchMask = oldArchMask | componentMask;

			auto& newArch = EnsureArchetype(newArchMask);
			Component& component = newArch.Emplace<Component>(ID, std::forward<Args>(args)...);

			if (oldArchMask) {
				// Move entity and its remaining components to the new archetype.
				auto& oldArch = m_Types.at(oldArchMask);
				oldArch.Move(ID, newArch);
				DestroyArchetypeIfEmpty(oldArchMask, oldArch);
			}

			// Update entity mask to reflect the change (added component)
			entityMask |= componentMask;

			return component;
		}

		template<typename... Components>
		std::tuple<Components&...> AddComponents(EntityID ID, Components&&... components) {
			ASSERT_VALID_ENTITY(ID);
			ARCH_ASSERT(!HasAnyComponent<Components...>(ID),
				"Entity already has atleast one of the specified components.");

			Bitmask& entityMask = m_Entities.at(ID);
			const Bitmask componentMask = CombineComponents<Components...>();
			const Bitmask oldArchMask = entityMask & ~ACTIVE_ENTITY;
			const Bitmask newArchMask = oldArchMask | componentMask;

			auto& newArch = EnsureArchetype(oldArchMask | componentMask);
			auto componentPack = newArch.AddMultiple<Components...>(
				ID, std::forward<Components>(components)...
			);

			if (oldArchMask) {
				auto& oldArch = m_Types.at(oldArchMask);
				oldArch.Move(ID, newArch);
				DestroyArchetypeIfEmpty(oldArchMask, oldArch);
			}

			entityMask |= componentMask;

			return componentPack;
		}

		template<typename Component>
		void RemoveComponent(EntityID ID) {
			RemoveComponents<Component>(ID);
		}

		template<typename... Components>
		void RemoveComponents(EntityID ID) {
			ASSERT_VALID_ENTITY(ID);
			ARCH_ASSERT(HasAllComponents<Components...>(ID),
				"Entity must have all of the specified components.");

			Bitmask& entityMask = m_Entities[ID];
			const Bitmask componentMask = CombineComponents<Components...>();
			const Bitmask oldArchMask = entityMask & ~ACTIVE_ENTITY;
			const Bitmask newArchMask = oldArchMask & ~componentMask;

			auto& oldArch = m_Types.at(oldArchMask);

			if (newArchMask)
				oldArch.Move(ID, EnsureArchetype(newArchMask));
			else
				oldArch.Delete(ID);

			DestroyArchetypeIfEmpty(oldArchMask, oldArch);

			entityMask &= ~componentMask;
		}

		template<typename Component>
		Component& GetComponent(EntityID ID) {
			ASSERT_VALID_ENTITY(ID);
			ARCH_ASSERT(HasComponent<Component>(ID),
				"Entity does not have the specified component");
			Bitmask mask = m_Entities[ID] & ~ACTIVE_ENTITY;

			return m_Types.at(mask).Get<Component>(ID);
		}

		template<typename... Components>
		std::tuple<Components&...> GetComponents(EntityID ID) {
			ASSERT_VALID_ENTITY(ID);
			ARCH_ASSERT(HasAllComponents<Components...>(ID),
				"Entity does not have all the specicied components");
			Bitmask mask = m_Entities[ID] & ~ACTIVE_ENTITY;

			return m_Types.at(mask).Multiple<Components...>(ID);
		}

		template<typename Component>
		bool HasComponent(EntityID ID) const {
			ASSERT_VALID_ENTITY(ID);
			return m_Entities[ID] & ComponentRegistry::GetMask<Component>();
		}

		template<typename... Components>
		bool HasAnyComponent(EntityID ID) const {
			return (HasComponent<Components>(ID) || ...);
		}

		template<typename... Components>
		bool HasAllComponents(EntityID ID) const {
			return (HasComponent<Components>(ID) && ...);
		}

		template<typename ...Components>
		Query<Components...> CreateQuery() {
			return { this };
		}

		void Reset() {
			m_MaxID = 0;
			m_Entities.clear();
			m_IdCache.clear();
			m_EntityToIndex.clear();
			m_Types.clear();
		}

		size_t GetEntityCount() const {
			return m_Entities.size() - m_IdCache.size();
		}

		size_t GetArchetypeCount() const {
			return m_Types.size();
		}
	};
}
#endif