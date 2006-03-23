public class UnexpectedParameterException extends BadInputException
{
	public final String name;
	public final int size;
	
	public UnexpectedParameterException(String name, int size, int offset)
	{
		super("Unexpected parameter: \"" + name + "\" of size " + size, offset);
		this.name = name;
		this.size = size;
	}
}
