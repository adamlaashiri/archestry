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
#define ARCH_MESSAGE(msg) std::cout << (msg) << '\n'
#else
#define ARCH_ASSERT(expr, msg) ((void)0)
#define ARCH_MESSAGE(msg) ((void)0)
#endif


namespace archestry {

	// EntityID is used to group and identify components.
	using EntityID = size_t;


	constexpr EntityID NULL_ENTITY = std::numeric_limits<EntityID>::max();


	/*
	* Each component has its unique bit.
	* The least significant bit represents the entity's state.
	* A combination of component bits form an archetype mask.
	*/
	using Bitmask = uint64_t;


	// First bit representing an entity's state
	constexpr Bitmask INACTIVE_ENTITY = 0;
	constexpr Bitmask ACTIVE_ENTITY = 1;


	constexpr size_t MAX_COMPONENT_TYPE_COUNT = sizeof(Bitmask) * CHAR_BIT - 1;


	using ConstructFn = void(*)(void* dst, void* src);
	using AssignFn = void(*)(void* dst, void* src);
	using DestructFn = void(*)(void* src);

	class ComponentRegistry;
	class Archetype;


	struct EntityMeta {
		Bitmask Mask = ACTIVE_ENTITY;
		Archetype* Archetype = nullptr;
		size_t Index = 0;
	};


	// Iterates over the set bits in a bitmask.
	struct BitmaskIterator {
		Bitmask Mask = 0;

		BitmaskIterator(Bitmask mask) : Mask(mask) {}

		bool HasNext() const {
			return Mask != 0;
		}

		Bitmask Next() {
			ARCH_ASSERT(Mask > 0, "Iterator Mask is 0");
			const Bitmask bit = Mask & -Mask;
			Mask ^= bit;
			return bit;
		}
	};


	inline size_t ComponentIndex(Bitmask mask) {
		ARCH_ASSERT(std::popcount(mask) == 1, "Not a single bit mask");
		return std::countr_zero(mask) - 1;
	}


	template<typename ...Components>
	Bitmask ComponentMask() {
		return (ComponentRegistry::template GetMask<Components>() | ... | 0);
	}


	struct ComponentMeta {
		size_t Size = 0;
		size_t Alignment = 0;
		bool IsTriviallyCopyable = false;
		ConstructFn MoveConstruct = nullptr;
		AssignFn MoveAssign = nullptr;
		DestructFn Destruct = nullptr;
	};


	/*
	* Handles registration and management of component types
	* and their associated metadata
	*/
	class ComponentRegistry {
	public:
		template<typename Component>
		static const Bitmask GetMask() {
			// Cache mask once
			static const Bitmask mask = RegisterType<Component>();
			return mask;
		}

		static const ComponentMeta& GetMeta(Bitmask mask) {
			ARCH_ASSERT(ComponentIndex(mask) < s_Counter,
				"Component with mask " << mask << " is not registered.");
			return s_Components[ComponentIndex(mask)];
		}
	private:
		inline static uint64_t s_Counter = 0;
		inline static std::array<ComponentMeta, MAX_COMPONENT_TYPE_COUNT> s_Components;

		template<typename Component>
		static const Bitmask RegisterType() {
			ARCH_ASSERT(s_Counter < MAX_COMPONENT_TYPE_COUNT,
				"Attempting to register more than " << MAX_COMPONENT_TYPE_COUNT << " components.");

			static_assert(!std::is_void_v<Component>, "Component type cannot be void.");

			/*
			* The component pool requires at least one of the two copy methods
			* to efficiently and safely resize its buffer and move components.
			* Non-trivial types must be both moveable and destructible
			*/

			constexpr bool isTrivialCopy = std::is_trivially_copyable_v<Component>;

			static_assert(isTrivialCopy || std::is_nothrow_move_constructible_v<Component>,
				"Component must be trivially copyable or have a non-throwing move constructor");

			if constexpr (!isTrivialCopy) {
				static_assert(std::is_nothrow_destructible_v<Component>,
					"Non-trivial component must be non-throwing destructible");

				static_assert(std::is_nothrow_move_assignable_v<Component>,
					"Non-trivial component must be non-throwing move assignable");
			}

			// First bit is reserved for the state of an entity, thus 2ULL
			const size_t index = s_Counter++;
			const Bitmask mask = 2ULL << index;

			s_Components[index] = ComponentMeta{
				sizeof(Component),
				alignof(Component),
				isTrivialCopy,
				!isTrivialCopy ? &MoveConstruct<Component> : nullptr,
				!isTrivialCopy ? &MoveAssign<Component> : nullptr,
				!isTrivialCopy ? &Destruct<Component> : nullptr
			};

			return mask;
		}

		template<typename Component>
		static void MoveConstruct(void* dst, void* src) {
			new (dst) Component(std::move(*static_cast<Component*>(src)));
		}

		template<typename Component>
		static void MoveAssign(void* dst, void* src) {
			*static_cast<Component*>(dst) = std::move(*static_cast<Component*>(src));
		}

		template<typename Component>
		static void Destruct(void* src) {
			static_cast<Component*>(src)->~Component();
		}
	};


	/*
	* Type erased pool that stores and
	* manages components contiguously in memory.
	*/
	class ComponentPool {
	private:
		// Self contained raw buffer
		class Buffer {
		private:
			size_t m_Alignment = 0;
			void* m_Data = nullptr;

			void Release() {
				::operator delete(m_Data, std::align_val_t(m_Alignment));
			}

		public:
			Buffer() = delete;

			Buffer(size_t size, size_t alignment) :
				m_Alignment(alignment),
				m_Data(::operator new(size, std::align_val_t(alignment))) {
			}

			Buffer& operator = (Buffer&& other) noexcept {
				if (this == &other)
					return *this;
				Release();
				m_Alignment = other.m_Alignment;
				m_Data = other.m_Data;
				other.m_Data = nullptr;
				return *this;
			}

			~Buffer() {
				Release();
			}

			void* Data() {
				return m_Data;
			}

			void* operator[](size_t offset) {
				return reinterpret_cast<void*>(
					reinterpret_cast<uintptr_t>(m_Data) + offset
					);
			}
		};

		// Method to move components between pools
		enum class CopyType {
			Memcpy, // memcpy
			Move // Move constructor & move assignment
		};

		const ComponentMeta m_Meta;
		const CopyType m_CopyType;
		Buffer m_Buffer;
		size_t m_Capacity = 0;
		size_t m_Size = 0;

		size_t ByteOffset(size_t offset) const {
			return m_Meta.Size * offset;
		}

		void Replace(size_t a, size_t b) {
			void* dst = m_Buffer[ByteOffset(a)];
			void* src = m_Buffer[ByteOffset(b)];

			switch (m_CopyType) {
			case CopyType::Memcpy:
				memcpy(dst, src, m_Meta.Size);
				break;
			case CopyType::Move:
				m_Meta.MoveAssign(dst, src);
				m_Meta.Destruct(src);
				break;
			default:
				ARCH_ASSERT(false, "Unsupported copy type");
			}
		}

		void Resize(size_t factor) {
			const size_t newCapacity = m_Capacity > 0 ? factor * m_Capacity : 1;
			Buffer newBuffer(newCapacity * m_Meta.Size, m_Meta.Alignment);

			switch (m_CopyType) {
			case CopyType::Memcpy: {
				void* dst = newBuffer.Data();
				void* src = m_Buffer.Data();
				memcpy(dst, src, m_Size * m_Meta.Size);
				break;
			}
			case CopyType::Move: {
				const size_t size = m_Size;
				for (size_t i = 0; i < size; i++) {
					void* dst = newBuffer[ByteOffset(i)];
					void* src = m_Buffer[ByteOffset(i)];
					m_Meta.MoveConstruct(dst, src);
					m_Meta.Destruct(src);
				}
				break;
			}
			default:
				ARCH_ASSERT(false, "Unsupported copy type");
			};

			m_Capacity = newCapacity;
			m_Buffer = std::move(newBuffer);
		}

		void EnsureSize() {
			if (m_Size >= m_Capacity) Resize(2);
		}

	public:
		ComponentPool() = delete;

		ComponentPool(ComponentMeta meta, size_t capacity) :
			m_Meta(meta),
			m_CopyType(m_Meta.IsTriviallyCopyable ? CopyType::Memcpy : CopyType::Move),
			m_Buffer(capacity* m_Meta.Size, m_Meta.Alignment),
			m_Capacity(capacity),
			m_Size(0) {
			// ASSERT valid componentInfo
		}

		ComponentPool(const ComponentPool& other) = delete;

		ComponentPool& operator=(const ComponentPool& other) = delete;

		~ComponentPool() {
			if (m_CopyType == CopyType::Move)
				for (size_t i = 0; i < m_Size; i++) {
					void* toDestroy = m_Buffer[ByteOffset(i)];
					m_Meta.Destruct(toDestroy);
				}
		}

		template<typename Component, typename... Args>
		Component& Emplace(Args&&... args) {
			EnsureSize();

			void* dst = m_Buffer[ByteOffset(m_Size)];
			new (dst) Component(std::forward<Args>(args)...);

			m_Size++;

			return *static_cast<Component*>(dst);
		}

		// Type erased component addition
		void* Add(void* component) {
			EnsureSize();

			void* dst = m_Buffer[ByteOffset(m_Size)];

			switch (m_CopyType) {
			case CopyType::Memcpy:
				memcpy(dst, component, m_Meta.Size);
				break;
			case CopyType::Move:
				m_Meta.MoveConstruct(dst, component);
				break;
			default:
				ARCH_ASSERT(false, "Unsupported copy type");
			};

			m_Size++;

			return dst;
		}

		void Delete(size_t index) {
			// Skip the replacement if the component is the last in the pool
			if (index != m_Size - 1)
				/*
				* Replace the component to be deleted with the last component and then delete it
				* to maintain continuous and tightly packed memory
				*/
				Replace(index, m_Size - 1);
			else if (!m_Meta.IsTriviallyCopyable)
				m_Meta.Destruct(m_Buffer[(m_Size - 1) * m_Meta.Size]);

			m_Size--;
		}

		template<typename T>
		T* GetBase() {
			return static_cast<T*>(m_Buffer.Data());
		}

		size_t GetSize() const {
			return m_Size;
		}

		void* operator [](size_t index) {
			ARCH_ASSERT(index < m_Size, "Index out of bounds: ");
			return m_Buffer[ByteOffset(index)];
		}
	};

	// Groups entities with the same set of components together.
		// Invariants:
		// - Registry outlives Archetype.
		// - All component pools have the same length.
		// - Always contains at least one entity.
		// - Validations happens upstream, no need to assert entity presence or component validity.
	class Archetype {
	private:
		const Bitmask m_ArchetypeMask = 0;

		EntityID m_LastAddedEntity = 0;

		/*
		* Tracks which components are still
		* expected to be added for last added entity
		*/
		Bitmask m_PendingMask = 0;

		size_t m_Size = 0;

		std::vector<EntityID> m_IndexToEntity;

		// m_Entities[index].index -> entity at index in component pool(s).
		std::vector<EntityMeta>& m_Entities;

		std::array<std::unique_ptr<ComponentPool>, MAX_COMPONENT_TYPE_COUNT> m_Pools;

		// Registers the entity once and tracks its components as they are added.
		void EnsureEntity(EntityID ID, Bitmask componentMask) {
			if (m_PendingMask == 0) {
				AssertSyncedPools();
				m_PendingMask = m_ArchetypeMask;
				m_LastAddedEntity = ID;
			}

			ARCH_ASSERT(ID == m_LastAddedEntity,
				"Different entity ID.");

			m_PendingMask &= ~componentMask;

			// Final component added.
			if (m_PendingMask == 0)
			{
				m_Entities[ID].Index = m_Size++;
				m_IndexToEntity.push_back(ID);
			}
		}

		bool ArePoolsSynced() const {
			for (BitmaskIterator it{ m_ArchetypeMask }; it.HasNext();)
				if (m_Size != m_Pools[ComponentIndex(it.Next())]->GetSize())
					return false;

			return true;
		}

		void AssertSyncedPools() const {
			ARCH_ASSERT(ArePoolsSynced(), "Component pools out of sync.");
		}

		template<typename Component>
		ARCH_FORCEINLINE ComponentPool& GetPool() {
			static const auto index = ComponentIndex(ComponentMask<Component>());
			return *m_Pools[index];
		}

	public:
		Archetype() = delete;

		Archetype(Bitmask archetypeMask, std::vector<EntityMeta>& entities) :
			m_ArchetypeMask(archetypeMask),
			m_Entities(entities) {

			for (BitmaskIterator it{ archetypeMask }; it.HasNext();) {
				const Bitmask componentMask = it.Next();
				m_Pools[ComponentIndex(componentMask)] =
					std::make_unique<ComponentPool>(
						ComponentRegistry::GetMeta(componentMask),
						1
					);
			}
		}

		Archetype(const Archetype& other) = delete;
		Archetype& operator=(const Archetype& other) = delete;

		template<typename Component, typename... Args>
		Component& Emplace(EntityID ID, Args&&... args) noexcept {
			EnsureEntity(ID, ComponentMask<Component>());
			return GetPool<Component>().Emplace<Component>(std::forward<Args>(args)...);
		}

		template<typename ...Components>
		std::tuple<Components&...> AddMultiple(EntityID ID, Components&&... components) {
			EnsureEntity(ID, ComponentMask<Components...>());
			return std::tuple<Components&...> {
				*static_cast<Components*>(GetPool<Components>().Add(&components))...
			};
		}

		template<typename Component>
		Component& Get(size_t index) {
			AssertSyncedPools();
			return GetPool<Component>().GetBase<Component>()[index];
		}

		template<typename ...Components>
		std::tuple<Components&...> GetMultiple(size_t index) {
			AssertSyncedPools();
			return std::tuple<Components&...>{
				GetPool<Components>().GetBase<Components>()[index]...
			};
		}

		template<typename ...Components>
		std::tuple<Components&...> First() {
			AssertSyncedPools();
			return std::tuple<Components&...> {
				*GetPool<Components>().GetBase<Components>()...
			};
		}

		template<typename ...Components, typename Fn>
		void ForEach(Fn&& fn) {
			AssertSyncedPools();

			auto typedPools = std::make_tuple(GetPool<Components>().GetBase<Components>()...);

			std::apply([&](auto... ptrs) {

				// This branch is for [](EntityID ID, Component& c1, Component& c2...);
				if constexpr (std::is_invocable_v<Fn&&, EntityID, Components&...>) {
					EntityID* indexToEntity = m_IndexToEntity.data();

					for (size_t i = 0; i < m_Size; i++)
						fn(indexToEntity[i], ptrs[i]...);
				}
				// This branch is for [](Component& c1, Component& c2...);
				else if constexpr (std::is_invocable_v<Fn&&, Components&...>) {
					for (size_t i = 0; i < m_Size; i++)
						fn(ptrs[i]...);
				}
				else
					ARCH_ASSERT(false,
						"ForEach() parameter pack does not match lambda args."
					);

				}, typedPools);
		}

		/*
		* Used in a type erased context.
		* Move every relevant component of entity to other archetype.
		* TODO: Move this responsibility to orchestrator.
		*/
		void Move(EntityID ID, Archetype& other) {
			AssertSyncedPools();
			other.AssertSyncedPools();

			const size_t index = m_Entities[ID].Index;

			for (BitmaskIterator it{ m_ArchetypeMask }; it.HasNext();) {
				const Bitmask componentMask = it.Next();
				const size_t componentIndex = ComponentIndex(componentMask);

				if (other.m_ArchetypeMask & componentMask) {
					other.EnsureEntity(ID, componentMask);
					other.m_Pools[componentIndex]->Add((*m_Pools[componentIndex])[index]);
				}
			}

			Delete(index);
		}

		void Delete(size_t index) {
			AssertSyncedPools();

			for (BitmaskIterator it{ m_ArchetypeMask }; it.HasNext();)
				m_Pools.at(ComponentIndex(it.Next()))->Delete(index);

			/*
			* Entity was already last in the pool; no replacement performed.
			* ComponentPool index mapping remains valid, no reassignment of index required.
			*/
			if (index == m_Size - 1) {
				m_IndexToEntity.pop_back();
				m_Size--;
				return;
			}

			// Replacement occured, update last entity to deleted entity's index.
			EntityID lastEntityID = m_IndexToEntity[m_Size - 1];
			m_Entities[lastEntityID].Index = index;
			m_IndexToEntity[index] = lastEntityID;
			m_IndexToEntity.pop_back();
			m_Size--;
		}

		Bitmask GetMask() const {
			return m_ArchetypeMask;
		}

		bool IsEmpty() const {
			AssertSyncedPools();
			return m_Size == 0;
		}

		size_t GetSize() const {
			AssertSyncedPools();
			return m_Size;
		}
	};


	// Main orchestrator of the ECS
	class Registry {
	private:
		template<typename...>
		friend class Query;

		std::vector<EntityMeta> m_Entities;
		std::vector<EntityID> m_IDCache;
		std::unordered_map<Bitmask, Archetype> m_Types;

		void AssertValidEntity(EntityID ID) const {
			ARCH_ASSERT(ID != NULL_ENTITY, "NULL_Entity");
			ARCH_ASSERT(ID < m_Entities.size(), "Entity with ID " << ID << " is invalid.");
			ARCH_ASSERT(m_Entities[ID].Mask != INACTIVE_ENTITY, "Entity with ID " << ID << " is inactive.");
		}

		Archetype& GetOrCreateArchetype(Bitmask mask) {
			return m_Types.try_emplace(mask, mask, m_Entities).first->second;
		}

		void DestroyArchetypeIfEmpty(Archetype& arch) {
			if (arch.IsEmpty()) m_Types.erase(arch.GetMask());
		}

		// Returns mask with cleared entity state bit
		Bitmask WithoutStateBit(Bitmask mask) const {
			return mask & ~ACTIVE_ENTITY;
		}
	public:
		Registry() = default;

		EntityID CreateEntity() {
			EntityID ID;
			if (m_IDCache.size()) {
				ID = m_IDCache.back();
				m_IDCache.pop_back();
				m_Entities[ID].Mask = ACTIVE_ENTITY;
			}
			else {
				ID = m_Entities.size();
				m_Entities.push_back(EntityMeta{});
			}

			return ID;
		}

		void DeleteEntity(EntityID ID) {
			AssertValidEntity(ID);

			auto& entity = m_Entities[ID];

			// Destroy associated components, if any
			if (entity.Archetype) {
				entity.Archetype->Delete(entity.Index);
				DestroyArchetypeIfEmpty(*entity.Archetype);
			}
			entity = { .Mask = INACTIVE_ENTITY };
			m_IDCache.push_back(ID);
		}

		template<typename Component, typename... Args>
		Component& AddComponent(EntityID ID, Args&&... args) {
			AssertValidEntity(ID);

			ARCH_ASSERT(!HasComponent<Component>(ID),
				"Entity already has the specified component.");

			EntityMeta& entity = m_Entities[ID];
			Bitmask componentMask = ComponentRegistry::GetMask<Component>();
			auto& newArch = GetOrCreateArchetype(WithoutStateBit(entity.Mask) | componentMask);

			if (entity.Archetype) {
				// Move entity components to the new archetype.
				entity.Archetype->Move(ID, newArch);
				DestroyArchetypeIfEmpty(*entity.Archetype);
			}

			Component& addedComponent = newArch.Emplace<Component>(ID, std::forward<Args>(args)...);

			entity.Mask |= componentMask;
			entity.Archetype = &newArch;

			return addedComponent;
		}

		template<typename... Components>
		std::tuple<Components&...> AddComponents(EntityID ID, Components&&... components) {
			AssertValidEntity(ID);
			ARCH_ASSERT(!HasAnyComponent<Components...>(ID),
				"Entity already has atleast one of the specified components.");

			EntityMeta& entity = m_Entities[ID];
			Bitmask componentMask = ComponentMask<Components...>();
			auto& newArch = GetOrCreateArchetype(WithoutStateBit(entity.Mask) | componentMask);

			if (entity.Archetype) {
				entity.Archetype->Move(ID, newArch);
				DestroyArchetypeIfEmpty(*entity.Archetype);
			}

			auto addedComponents = newArch.AddMultiple<Components...>(ID, std::forward<Components>(components)...);

			entity.Mask |= componentMask;
			entity.Archetype = &newArch;


			return addedComponents;
		}

		template<typename Component>
		void RemoveComponent(EntityID ID) {
			RemoveComponents<Component>(ID);
		}

		template<typename... Components>
		void RemoveComponents(EntityID ID) {
			AssertValidEntity(ID);
			ARCH_ASSERT(HasAllComponents<Components...>(ID),
				"Entity must have all of the specified components.");

			EntityMeta& entity = m_Entities[ID];
			Bitmask componentMask = ComponentMask<Components...>();
			Bitmask newArchMask = WithoutStateBit(entity.Mask) & ~componentMask;

			auto& oldArch = *entity.Archetype;

			if (newArchMask) {
				auto& newArch = GetOrCreateArchetype(newArchMask);
				oldArch.Move(ID, newArch);
				entity.Archetype = &newArch;
			}
			else {
				oldArch.Delete(entity.Index);
				entity = { .Mask = entity.Mask };
			}

			DestroyArchetypeIfEmpty(oldArch);

			entity.Mask &= ~componentMask;
		}

		template<typename Component>
		Component& GetComponent(EntityID ID) {
			AssertValidEntity(ID);
			ARCH_ASSERT(HasComponent<Component>(ID), "Entity does not have the specified component.");
			EntityMeta& entity = m_Entities[ID];
			return entity.Archetype->Get<Component>(entity.Index);
		}

		template<typename... Components>
		std::tuple<Components&...> GetComponents(EntityID ID) {
			AssertValidEntity(ID);
			ARCH_ASSERT(HasAllComponents<Components...>(ID), "Entity does not have all the specicied components.");
			EntityMeta& entity = m_Entities[ID];
			return entity.Archetype->GetMultiple<Components...>(entity.Index);
		}

		template<typename Component>
		bool HasComponent(EntityID ID) const {
			AssertValidEntity(ID);
			return m_Entities[ID].Mask & ComponentMask<Component>();
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
			m_Entities.clear();
			m_IDCache.clear();
			m_Types.clear();
		}

		size_t GetEntityCount() const {
			return m_Entities.size() - m_IDCache.size();
		}

		size_t GetArchetypeCount() const {
			return m_Types.size();
		}
	};


	/*
	* Provides self contained queries defined
	* by the passed-in component parameter pack.
	*/
	template<typename ...Components>
	class Query {
	private:
		Registry* m_Registry = nullptr;

		Bitmask m_IncludedMask = 0;

		Bitmask m_ExcludedMask = 0;

		bool IsTarget(Bitmask mask) const {
			if ((mask & m_IncludedMask) != m_IncludedMask ||
				(mask & m_ExcludedMask) > 0)
				return false;

			return true;
		}

		void AssertValidRegistry() const {
			ARCH_ASSERT(m_Registry != nullptr, "Invalid pointer to Registry");
		}

	public:
		Query(Registry* registry) :
			m_Registry(registry),
			m_IncludedMask(ComponentMask<Components...>()) {

			static_assert(sizeof...(Components) != 0,
				"Query requires at least one component.");

		}

		template<typename ...ExcludedComponents>
		Query& Without() {
			m_ExcludedMask |=
				ComponentMask<ExcludedComponents...>();

			return *this;
		}

		bool HasEntity(EntityID ID) const {
			AssertValidRegistry();
			m_Registry->AssertValidEntity(ID);

			return m_Registry->HasAllComponents<Components...>(ID) &&
				((m_Registry->m_Entities[ID].Mask & m_ExcludedMask) == 0);
		}

		std::optional<std::tuple<Components&...>> GetFirst() {
			AssertValidRegistry();

			for (auto& [mask, archetype] : m_Registry->m_Types) {
				if (!IsTarget(mask))
					continue;
				return archetype.First<Components...>();
			}

			return std::nullopt;
		}

		template<typename Fn>
		void ForEach(Fn&& fn) {
			AssertValidRegistry();

			for (auto& [mask, archetype] : m_Registry->m_Types) {
				if (!IsTarget(mask))
					continue;

				archetype.ForEach<Components...>(std::forward<Fn>(fn));
			}
		}
	};

}
#endif