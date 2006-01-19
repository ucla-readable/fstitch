/**
 * A GrouperFactory creates new instances of a Grouper.
 * Grouper Factories are useful for nested Groupers.
 */
public interface GrouperFactory
{
	Grouper newInstance();
}
