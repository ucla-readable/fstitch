public class MissingParameterException extends BadInputException
{
	public final String name;
	public final int size;
	
	public MissingParameterException(String name, int size, int offset)
	{
		super("Missing parameter: \"" + name + "\" of size " + size, offset);
		this.name = name;
		this.size = size;
	}
}
