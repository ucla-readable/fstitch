public class UnexpectedParameterException extends BadInputException
{
	private final String name;
	private final int size;
	
	public UnexpectedParameterException(String name, int size)
	{
		super("Unexpected parameter: \"" + name + "\" of size " + size);
		this.name = name;
		this.size = size;
	}
	
	public String getName()
	{
		return name;
	}
	
	public int  getSize()
	{
		return size;
	}
}
