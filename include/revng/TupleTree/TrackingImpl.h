#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <cstdint>
#include <tuple>

#include "revng/ADT/KeyedObjectContainer.h"
#include "revng/ADT/UpcastablePointer.h"
#include "revng/Support/TrackingSession.h"
#include "revng/TupleTree/Tracking.h"
#include "revng/TupleTree/TupleTreePath.h"
#include "revng/TupleTree/Visits.h"

namespace revng {

struct TrackingImpl {
  struct PopVisitor {
    template<revng::SetOrKOC Type>
    static void visitKeyedObjectContainer(const Type &CurrentItem) {
      CurrentItem.trackingPop();
    }

    template<typename Type, size_t FieldIndex>
    static void visitTupleElement(const Type &CurrentItem) {
      CurrentItem.template getTracker<FieldIndex>().pop();
    }
  };

  struct PushVisitor {
    template<revng::SetOrKOC Type>
    static void visitKeyedObjectContainer(const Type &CurrentItem) {
      CurrentItem.trackingPush();
    }

    template<typename Type, size_t FieldIndex>
    static void visitTupleElement(const Type &CurrentItem) {
      CurrentItem.template getTracker<FieldIndex>().push();
    }
  };

  struct ClearVisitor {
    template<revng::SetOrKOC Type>
    static void visitKeyedObjectContainer(const Type &CurrentItem) {
      CurrentItem.clearTracking();
    }

    template<typename Type, size_t FieldIndex>
    static void visitTupleElement(const Type &CurrentItem) {
      CurrentItem.template getTracker<FieldIndex>().clear();
    }
  };

  struct StopTrackingVisitor {
    template<revng::SetOrKOC Type>
    static void visitKeyedObjectContainer(const Type &CurrentItem) {
      CurrentItem.stopTracking();
    }

    template<typename Type, size_t FieldIndex>
    static void visitTupleElement(const Type &CurrentItem) {
      CurrentItem.template getTracker<FieldIndex>().stopTracking();
    }
  };

  // Uncomment the following for easier debugging:
  // #define RECURSIVE_COLLECT_TUPLE

#ifdef RECURSIVE_COLLECT_TUPLE
  template<typename M, size_t I = 0, typename T>
  static void
  collectTuple(const T &LHS, TupleTreePath &Stack, ReadFields &Info) {
    if constexpr (I < std::tuple_size_v<T>) {

      Stack.push_back(size_t(I));
      if (LHS.template getTracker<I>().isSet()) {
        Info.Read.push_back(Stack);
      }
      collectImpl<M>(LHS.template untrackedGet<I>(), Stack, Info);
      Stack.pop_back();

      // Recur
      collectTuple<M, I + 1>(LHS, Stack, Info);
    }
  }
#endif

  template<typename M, StrictSpecializationOf<UpcastablePointer> T>
  static void collectImpl(const T &UP, TupleTreePath &Stack, ReadFields &Info) {
    if (!UP.isEmpty()) {
      auto KindTrackingSuspender = UP.get()->KindTracker.suspend();
      UP.upcast([&](auto &Upcasted) {
        // Don't forget to add the kind of the polymorphic object to the stack.
        static_assert(!std::is_const_v<
                      std::remove_reference_t<decltype(Upcasted)>>);
        Stack.push_back(Upcasted.Kind());
        collectImpl<M>(Upcasted, Stack, Info);
        Stack.pop_back();
      });
    }
  }

#ifdef RECURSIVE_COLLECT_TUPLE
  template<typename M, TupleSizeCompatible T>
  static void
  collectImpl(const T &LHS, TupleTreePath &Stack, ReadFields &Info) {
    collectTuple<M>(LHS, Stack, Info);
  }

#else
  // collectImpl remains unchanged
  template<typename M, TupleSizeCompatible T>
  static void
  collectImpl(const T &LHS, TupleTreePath &Stack, ReadFields &Info) {
    collectTuple<M>(LHS, Stack, Info);
  }

  // Helper function with index sequence
  template<typename M, typename T, size_t... Is>
  static void collectTupleImpl(const T &LHS,
                               TupleTreePath &Stack,
                               ReadFields &Info,
                               std::index_sequence<Is...>) {
    // Use a fold expression to process each index
    (...,
     (Stack.push_back(Is),
      (LHS.template getTracker<Is>().isSet() ?
         (Info.Read.push_back(Stack), void()) :
         void()),
      collectImpl<M>(LHS.template untrackedGet<Is>(), Stack, Info),
      Stack.pop_back()));
  }

  // Main function using index sequence
  template<typename M, typename T>
  static void
  collectTuple(const T &LHS, TupleTreePath &Stack, ReadFields &Info) {
    collectTupleImpl<M>(LHS,
                        Stack,
                        Info,
                        std::make_index_sequence<std::tuple_size_v<T>>{});
  }
#endif

  template<typename M, revng::SetOrKOC T>
  static void
  collectImpl(const T &LHS, TupleTreePath &Stack, ReadFields &Info) {
    typename T::TrackingResult TrackingResult = LHS.getTrackingResult();
    if (TrackingResult.Exact)
      Info.ExactVectors.push_back(Stack);

    using KeyType = std::remove_cv_t<typename T::key_type>;
    for (KeyType Key : TrackingResult.InspectedKeys) {
      Stack.push_back(Key);
      Info.Read.push_back(Stack);
      Stack.pop_back();
    }
    for (auto &LHSElement : LHS.Content) {
      using value_type = typename T::value_type;

      using Type = decltype(LHSElement);
      auto &Mutable = const_cast<std::remove_cvref_t<Type> &>(LHSElement);
      static_assert(!std::is_const_v<
                    std::remove_reference_t<decltype(Mutable)>>);
      Stack.push_back(KeyedObjectTraits<value_type>::key(Mutable));

      collectImpl<M>(LHSElement, Stack, Info);
      Stack.pop_back();
    }
  }

  template<typename M, NotTupleTreeCompatible T>
  static void
  collectImpl(const T &LHS, TupleTreePath &Stack, ReadFields &Info) {}

  template<typename M, typename Visitor, size_t I = 0, typename T>
  static void visitTuple(const T &LHS) {
    if constexpr (I < std::tuple_size_v<T>) {

      visitImpl<M, Visitor>(LHS.template untrackedGet<I>());
      Visitor::template visitTupleElement<T, I>(LHS);

      // Recur
      visitTuple<M, Visitor, I + 1, T>(LHS);
    }
  }

  template<typename M,
           typename Visitor,
           StrictSpecializationOf<UpcastablePointer> T>
  static void visitImpl(T &UP) {
    if (!UP.isEmpty()) {
      auto KindTrackingSuspender = UP.get()->KindTracker.suspend();
      UP.upcast([&](const auto &Upcasted) { visitImpl<M, Visitor>(Upcasted); });
    }
  }

  template<typename M, typename Visitor, TupleSizeCompatible T>
  static void visitImpl(const T &LHS) {
    visitTuple<M, Visitor>(LHS);
  }

  template<typename M, typename Visitor, revng::SetOrKOC T>
  static void visitImpl(const T &LHS) {
    for (auto &LHSElement : LHS.Content) {
      visitImpl<M, Visitor>(LHSElement);
    }
    Visitor::template visitKeyedObjectContainer<T>(LHS);
  }

  template<typename M, typename Visitor, NotTupleTreeCompatible T>
  static void visitImpl(const T &LHS) {}

  //
  // Index of the objects of a tree for a TrackingSession. It follows the
  // traversal of collectImpl, and each object's kind appends the same path
  // elements collectImpl pushes when it reaches the object.
  //

  using Session = TrackingSession;
  using Object = TrackingSession::Object;
  using ObjectKind = TrackingSession::ObjectKind;

  static void appendNothing(const Object &, TupleTreePath &) {}

  static void appendField(const Object &Target, TupleTreePath &Path) {
    Path.push_back(static_cast<size_t>(Target.Field));
  }

  template<StrictSpecializationOf<UpcastablePointer> T>
  static void appendKind(const T &UP, TupleTreePath &Path) {
    auto KindTrackingSuspender = UP.get()->KindTracker.suspend();
    UP.upcast([&](auto &Upcasted) { Path.push_back(Upcasted.Kind()); });
  }

  template<StrictSpecializationOf<UpcastablePointer> T>
  static void appendFieldAndKind(const Object &Target, TupleTreePath &Path) {
    appendField(Target, Path);
    appendKind(*static_cast<const T *>(Target.Source), Path);
  }

  template<revng::SetOrKOC Container>
  static void appendKey(const Object &Target, TupleTreePath &Path) {
    using value_type = typename Container::value_type;
    auto &Element = *static_cast<const value_type *>(Target.Source);
    auto &Mutable = const_cast<value_type &>(Element);
    Path.push_back(KeyedObjectTraits<value_type>::key(Mutable));
  }

  template<revng::SetOrKOC Container>
  static void appendKeyAndKind(const Object &Target, TupleTreePath &Path) {
    using value_type = typename Container::value_type;
    appendKey<Container>(Target, Path);
    appendKind(*static_cast<const value_type *>(Target.Source), Path);
  }

  template<typename T, size_t... Is>
  static int32_t
  fieldOfImpl(const T &Value, const AccessTracker *Tracker,
              std::index_sequence<Is...>) {
    int32_t Result = Session::NoField;
    (void) ((&Value.template getTracker<Is>() == Tracker ?
               (Result = static_cast<int32_t>(Is), true) :
               false)
            or ...);
    return Result;
  }

  template<typename T>
  static int32_t tupleFieldOf(const Object &Target,
                              const AccessTracker *Tracker) {
    const T &Value = *reinterpret_cast<const T *>(Target.Begin);
    return fieldOfImpl(Value,
                       Tracker,
                       std::make_index_sequence<std::tuple_size_v<T>>{});
  }

  template<typename T, auto Append>
  static constexpr ObjectKind TupleKind = {
    .appendElements = Append,
    .fieldOf = &tupleFieldOf<T>,
    .pushLevel = nullptr,
    .popLevel = nullptr,
    .forgetLevels = nullptr,
    .hasEmptyLevels = nullptr,
    .membership = nullptr,
    .collectKeys = nullptr,
  };

  template<revng::SetOrKOC T>
  static const T &container(const Object &Target) {
    return *reinterpret_cast<const T *>(Target.Begin);
  }

  template<revng::SetOrKOC T>
  static int32_t containerFieldOf(const Object &Target,
                                  const AccessTracker *Tracker) {
    if (&container<T>(Target).Exact == Tracker)
      return Session::ExactField;
    return Session::NoField;
  }

  template<revng::SetOrKOC T>
  static void collectKeys(const Object &Target,
                          const TupleTreePath &Path,
                          llvm::SmallVectorImpl<TupleTreePath> &Read) {
    typename T::TrackingResult Result = container<T>(Target)
                                          .getTrackingResult();
    using KeyType = std::remove_cv_t<typename T::key_type>;
    for (KeyType Key : Result.InspectedKeys) {
      TupleTreePath KeyPath = Path;
      KeyPath.push_back(Key);
      Read.push_back(std::move(KeyPath));
    }
  }

  template<revng::SetOrKOC T>
  static constexpr ObjectKind ContainerKind = {
    .appendElements = &appendField,
    .fieldOf = &containerFieldOf<T>,
    .pushLevel = [](const Object &O) { container<T>(O).pushLevel(); },
    .popLevel = [](const Object &O) { container<T>(O).popLevel(); },
    .forgetLevels = [](const Object &O) { container<T>(O).forgetLevels(); },
    .hasEmptyLevels =
      [](const Object &O, size_t Levels) {
        return container<T>(O).hasEmptyLevels(Levels);
      },
    .membership =
      [](const Object &O) -> TrackingSessionMembership & {
        return container<T>(O).membership();
      },
    .collectKeys = &collectKeys<T>,
  };

  template<typename T>
  static Object makeObject(const T &Value,
                           const void *Source,
                           uint32_t Parent,
                           uint32_t Enclosing,
                           uint32_t Field,
                           const ObjectKind &Kind) {
    return Object{ .Begin = reinterpret_cast<const char *>(&Value),
                   .Size = sizeof(T),
                   .Source = Source,
                   .Parent = Parent,
                   .Enclosing = Enclosing,
                   .Field = Field,
                   .Kind = &Kind };
  }

  static void indexTracker(Session &S,
                           AccessTracker &Tracker,
                           uint32_t Owner,
                           int32_t Field) {
    Tracker.membership().set(TrackingSessionMembership::Unknown);
    if (Tracker.isSet())
      S.listTracker(Tracker, Owner, Field);
  }

  template<typename T, size_t... Is>
  static void indexTupleImpl(Session &S,
                             const T &Value,
                             uint32_t Self,
                             std::index_sequence<Is...>) {
    (...,
     (indexTracker(S, Value.template getTracker<Is>(), Self, Is),
      indexField(S, Value.template untrackedGet<Is>(), Self, Is)));
  }

  template<typename T>
  static void indexTuple(Session &S, const T &Value, uint32_t Self) {
    indexTupleImpl(S,
                   Value,
                   Self,
                   std::make_index_sequence<std::tuple_size_v<T>>{});
  }

  template<typename T>
  static void
  indexField(Session &S, const T &Value, uint32_t Parent, uint32_t Field) {
    if constexpr (StrictSpecializationOf<T, UpcastablePointer>) {
      if (Value.isEmpty())
        return;
      auto KindTrackingSuspender = Value.get()->KindTracker.suspend();
      Value.upcast([&](auto &Upcasted) {
        using U = std::remove_cvref_t<decltype(Upcasted)>;
        constexpr const ObjectKind &Kind = TupleKind<U, &appendFieldAndKind<T>>;
        auto Self = S.addObject(makeObject(Upcasted,
                                           &Value,
                                           Parent,
                                           Session::None,
                                           Field,
                                           Kind));
        indexTuple(S, Upcasted, Self);
      });
    } else if constexpr (TupleSizeCompatible<T>) {
      constexpr const ObjectKind &Kind = TupleKind<T, &appendField>;
      auto Self = S.addObject(makeObject(Value,
                                         &Value,
                                         Parent,
                                         Parent,
                                         Field,
                                         Kind));
      indexTuple(S, Value, Self);
    } else if constexpr (revng::SetOrKOC<T>) {
      auto Self = S.addObject(makeObject(Value,
                                         &Value,
                                         Parent,
                                         Parent,
                                         Field,
                                         ContainerKind<T>));
      indexTracker(S, Value.Exact, Self, Session::ExactField);
      Value.membership().set(TrackingSessionMembership::Unknown);
      for (auto &Element : Value.Content)
        indexElement<T>(S, Element, Self);
    }
  }

  template<revng::SetOrKOC Container, typename T>
  static void indexElement(Session &S, const T &Element, uint32_t Parent) {
    if constexpr (StrictSpecializationOf<T, UpcastablePointer>) {
      if (Element.isEmpty())
        return;
      auto KindTrackingSuspender = Element.get()->KindTracker.suspend();
      Element.upcast([&](auto &Upcasted) {
        using U = std::remove_cvref_t<decltype(Upcasted)>;
        constexpr const ObjectKind
          &Kind = TupleKind<U, &appendKeyAndKind<Container>>;
        auto Self = S.addObject(makeObject(Upcasted,
                                           &Element,
                                           Parent,
                                           Session::None,
                                           Session::None,
                                           Kind));
        indexTuple(S, Upcasted, Self);
      });
    } else if constexpr (TupleSizeCompatible<T>) {
      constexpr const ObjectKind &Kind = TupleKind<T, &appendKey<Container>>;
      auto Self = S.addObject(makeObject(Element,
                                         &Element,
                                         Parent,
                                         Session::None,
                                         Session::None,
                                         Kind));
      indexTuple(S, Element, Self);
    }
  }

  template<typename M>
  static void indexRoot(Session &S, const M &Root) {
    constexpr const ObjectKind &Kind = TupleKind<M, &appendNothing>;
    auto Self = S.addObject(makeObject(Root,
                                       &Root,
                                       Session::None,
                                       Session::None,
                                       Session::None,
                                       Kind));
    indexTuple(S, Root, Self);
    S.finishIndex();
  }
};

template<typename M>
ReadFields Tracking::collect(const M &LHS) {
  ReadFields Info;
  if (TrackingSession *Session = TrackingSession::find(&LHS)) {
    Session->collect(Info.Read,
                     Info.ExactVectors,
                     Info.Coarse,
                     ReadFields::CoarseThreshold);
    Info.deduplicate();
    return Info;
  }

  TrackingImpl::visitTuple<M, TrackingImpl::StopTrackingVisitor>(LHS);
  TupleTreePath Stack;
  TrackingImpl::collectImpl<M>(LHS, Stack, Info);
  TrackingImpl::visitTuple<M, TrackingImpl::ClearVisitor>(LHS);
  Info.coarsen();
  Info.deduplicate();
  return Info;
}

template<typename M>
void Tracking::clearAndResume(const M &LHS) {
  if (TrackingSession *Session = TrackingSession::find(&LHS)) {
    Session->clear();
    return;
  }

  TrackingImpl::visitTuple<M, TrackingImpl::ClearVisitor>(LHS);
}

template<typename M>
void Tracking::openSession(const M &LHS) {
  if (TrackingSession *Session = TrackingSession::open(&LHS))
    TrackingImpl::indexRoot(*Session, LHS);
}

template<typename M>
void Tracking::push(const M &LHS) {
  if (TrackingSession *Session = TrackingSession::find(&LHS))
    Session->push();
  else
    TrackingImpl::visitTuple<M, TrackingImpl::PushVisitor>(LHS);
}

template<typename M>
void Tracking::pop(const M &LHS) {
  if (TrackingSession *Session = TrackingSession::find(&LHS))
    Session->pop();
  else
    TrackingImpl::visitTuple<M, TrackingImpl::PopVisitor>(LHS);
}

template<typename M>
void Tracking::stop(const M &LHS) {
  TrackingImpl::visitTuple<M, TrackingImpl::StopTrackingVisitor>(LHS);
  TrackingSession::close(&LHS);
}

} // namespace revng
